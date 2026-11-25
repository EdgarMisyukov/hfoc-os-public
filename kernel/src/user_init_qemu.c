#if defined(HFOC_USER_INIT_QEMU_TEST) || defined(HFOC_USER_INIT_HOST_TEST)

#include "user_init.h"

#ifdef HFOC_USER_INIT_QEMU_TEST
#include "audit.h"
#include "event_ring.h"
#include "image_integrity.h"
#include "kernel_vfs.h"
#include "pipe.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "serial.h"
#include "system_image.h"
#include "syscall.h"
#include "user.h"
#include "user_process.h"
#include "vfs.h"
#include "vmm.h"
#endif

#include <stddef.h>
#include <stdint.h>
#include <string.h>

__asm__(
".text\n"
".global user_init_image_code_start\n"
".type user_init_image_code_start, @function\n"
"user_init_image_code_start:\n"
"    mov %cs, %ax\n"
"    mov %ax, user_init_image_marker_cs(%rip)\n"
"    movl $0x1E571E57, user_init_image_marker_magic(%rip)\n"
"    movl $0, %eax\n"
"    movl $1, %edi\n"
"    leaq user_init_image_marker_cs(%rip), %rsi\n"
"    movl $6, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_write(%rip)\n"
"    movl $1, %eax\n"
"    xorl %edi, %edi\n"
"    xorl %esi, %esi\n"
"    xorl %edx, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_yield(%rip)\n"
"    movl $4, %eax\n"
"    leaq user_init_image_fs_path(%rip), %rdi\n"
"    movl $15, %esi\n"
"    xorl %edx, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_open(%rip)\n"
"    movl $14, %eax\n"
"    movq user_init_image_marker_open(%rip), %rdi\n"
"    leaq user_init_image_fs_text(%rip), %rsi\n"
"    movl $5, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_fswrite(%rip)\n"
"    movl $8, %eax\n"
"    movq user_init_image_marker_open(%rip), %rdi\n"
"    xorl %esi, %esi\n"
"    xorl %edx, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_seek(%rip)\n"
"    movl $3, %eax\n"
"    movq user_init_image_marker_open(%rip), %rdi\n"
"    leaq user_init_image_fs_buffer(%rip), %rsi\n"
"    movl $5, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_fsread(%rip)\n"
"    movl $5, %eax\n"
"    movq user_init_image_marker_open(%rip), %rdi\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_fsclose(%rip)\n"
"    movl $2, %eax\n"
"    xorl %edi, %edi\n"
"    xorl %esi, %esi\n"
"    xorl %edx, %edx\n"
"    syscall\n"
"    movq %rax, user_init_image_marker_exit(%rip)\n"
"1:\n"
"    hlt\n"
"    jmp 1b\n"
"user_init_image_marker_cs:\n"
"    .word 0\n"
"user_init_image_marker_magic:\n"
"    .long 0\n"
"user_init_image_marker_write:\n"
"    .quad 0\n"
"user_init_image_marker_yield:\n"
"    .quad 0\n"
"user_init_image_marker_exit:\n"
"    .quad 0\n"
"user_init_image_marker_open:\n"
"    .quad 0\n"
"user_init_image_marker_fswrite:\n"
"    .quad 0\n"
"user_init_image_marker_seek:\n"
"    .quad 0\n"
"user_init_image_marker_fsread:\n"
"    .quad 0\n"
"user_init_image_marker_fsclose:\n"
"    .quad 0\n"
"user_init_image_fs_path:\n"
"    .asciz \"/tmp/fs\"\n"
"user_init_image_fs_text:\n"
"    .ascii \"hello\"\n"
"user_init_image_fs_buffer:\n"
"    .skip 8\n"
".global user_init_image_code_end\n"
"user_init_image_code_end:\n"
);

extern char user_init_image_code_start[];
extern char user_init_image_code_end[];

static void user_init_zero_bytes(void *destination, size_t count) {
    unsigned char *bytes = (unsigned char *)destination;

    while (count != 0U) {
        --count;
        bytes[count] = 0U;
    }
}

size_t user_init_image_size(void) {
    return (size_t)((uintptr_t)user_init_image_code_end -
                    (uintptr_t)user_init_image_code_start);
}

bool user_init_image_copy(uint8_t *dest, size_t capacity) {
    const size_t size = user_init_image_size();

    if (dest == NULL || capacity == 0U || capacity < size) {
        return false;
    }
    memcpy(dest, user_init_image_code_start, size);
    if (capacity > size) {
        user_init_zero_bytes(dest + size, capacity - size);
    }
    return true;
}

const char *user_init_status_string(enum user_init_status status) {
    switch (status) {
    case USER_INIT_OK:
        return "USER_INIT_OK";
    case USER_INIT_NULL:
        return "USER_INIT_NULL";
    case USER_INIT_IMAGE_TOO_LARGE:
        return "USER_INIT_IMAGE_TOO_LARGE";
    default:
        return "USER_INIT_UNKNOWN";
    }
}

#ifdef HFOC_USER_INIT_QEMU_TEST

static const struct system_image_manifest *user_init_integrity_manifest(void) {
    static const struct system_image_manifest manifest = {
        .manifest_version = SYSTEM_IMAGE_MANIFEST_VERSION,
        .image_id = 7U,
        .min_kernel_abi = SYSTEM_IMAGE_KERNEL_ABI_MIN,
        .max_kernel_abi = SYSTEM_IMAGE_KERNEL_ABI_MAX,
        .arch = SYSTEM_IMAGE_ARCH_VALUE,
        .component_count = 1U,
        .components[0] = {
            .name = "initramfs",
            .version = "1.0",
            .hash =
                "1f31a9eea1dccd472669347638ad3dca9cf18a726ac1491be9c2c276116ced47",
        },
    };

    return &manifest;
}

static struct kernel_process *user_init_parent;
static struct kernel_process *user_init_child;
static uint64_t user_init_hhdm;
static uint32_t user_init_child_pid;
static struct kernel_process *user_init_elf_child;
static uint32_t user_init_elf_pid;
static const char *const user_init_elf_argv[] = { "hello", "arg", NULL };
static struct kernel_process *user_init_crash_child;
static uint32_t user_init_crash_pid;

static void user_init_failure(const char *message) {
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[USER-INIT] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void user_init_audit_observability_proof(void) {
    struct event_ring *ring = kernel_event_ring_get();
    struct audit_record records[5];
    struct audit_record extra;
    uint32_t index;

    if (ring == NULL || !event_ring_is_published(ring)) {
        user_init_failure("audit ring");
    }
    serial_write("[AUDIT-OBS] consumer start\n");
    for (index = 0U; index < 5U; ++index) {
        const enum audit_status pop_status =
            audit_record_pop(ring, &records[index]);

        if (pop_status != AUDIT_OK) {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[AUDIT-OBS] failed: pop status ");
                serial_write(audit_status_string(pop_status));
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            user_init_failure("audit pop");
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[AUDIT-OBS] consumed index=");
            serial_write_u64(index);
            serial_write(" sequence=");
            serial_write_u64(records[index].sequence);
            serial_write(" type=");
            serial_write_u64(records[index].type);
            serial_write(" operation=");
            serial_write_u64(records[index].operation);
            serial_write(" result=");
            serial_write_u64(records[index].result);
            serial_write(" pid=");
            serial_write_u64(records[index].pid);
            serial_write(" domain=");
            serial_write_u64(records[index].domain);
            serial_write(" generation=");
            serial_write_u64(records[index].generation);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }

    if (records[0].sequence != 0U ||
        records[0].type != AUDIT_RECORD_SYSCALL ||
        records[0].operation != SYSCALL_WRITE ||
        records[0].result != 6U ||
        records[0].pid != user_init_child_pid ||
        records[0].domain != PROCESS_SECURITY_DOMAIN_USER ||
        records[0].generation != 0U ||
        records[0].detail != 0U) {
        user_init_failure("audit write fields");
    }
    if (records[1].sequence != 1U ||
        records[1].type != AUDIT_RECORD_SYSCALL ||
        records[1].operation != SYSCALL_YIELD ||
        records[1].result != SYSCALL_RET_OK ||
        records[1].pid != user_init_child_pid ||
        records[1].domain != PROCESS_SECURITY_DOMAIN_USER ||
        records[1].generation != 0U ||
        records[1].detail != 0U) {
        user_init_failure("audit yield fields");
    }
    if (records[2].sequence != 2U ||
        records[2].type != AUDIT_RECORD_SYSCALL ||
        records[2].operation != SYSCALL_EXIT ||
        records[2].result != SYSCALL_RET_OK ||
        records[2].pid != user_init_child_pid ||
        records[2].domain != PROCESS_SECURITY_DOMAIN_USER ||
        records[2].generation != 0U ||
        records[2].detail != 0U) {
        user_init_failure("audit exit fields");
    }
    if (records[3].sequence != 3U ||
        records[3].type != AUDIT_RECORD_SYSCALL ||
        records[3].operation != SYSCALL_WRITE ||
        records[3].result != 11U ||
        records[3].pid != user_init_elf_pid ||
        records[3].domain != PROCESS_SECURITY_DOMAIN_USER ||
        records[3].generation != 0U) {
        user_init_failure("audit elf write fields");
    }
    if (records[4].sequence != 4U ||
        records[4].type != AUDIT_RECORD_SYSCALL ||
        records[4].operation != SYSCALL_EXIT ||
        records[4].result != SYSCALL_RET_OK ||
        records[4].pid != user_init_elf_pid ||
        records[4].domain != PROCESS_SECURITY_DOMAIN_USER ||
        records[4].generation != 0U) {
        user_init_failure("audit elf exit fields");
    }
    if (audit_record_pop(ring, &extra) != AUDIT_EMPTY) {
        user_init_failure("audit extra");
    }
    if (event_ring_count(ring) != 0U) {
        user_init_failure("audit ring count");
    }
    serial_write("[AUDIT-OBS] empty after exact consume\n");
    serial_write("[AUDIT-OBS] parent consumed exactly once\n");
}

static void user_init_reap_log(const char *label, uint32_t pid,
                               uint32_t exit_status) {
    const uint64_t serial_flags = serial_sequence_begin();

    serial_write("[USER-EXEC] ");
    serial_write(label);
    serial_write(" reaped pid=");
    serial_write_u64(pid);
    serial_write(" status=");
    serial_write_u64(exit_status);
    serial_write(" free=");
    serial_write_u64(pmm_free_page_count());
    serial_write("\n");
    serial_sequence_end(serial_flags);
}

static void user_init_reap_child(uint32_t expected_pid,
                                 uint32_t expected_status,
                                 const char *label) {
    for (;;) {
        uint32_t pid = 0U;
        uint32_t exit_status = 0U;
        const enum process_status wait_status =
            process_wait_kernel(&user_init_parent->pcb, &pid, &exit_status,
                                user_init_hhdm);

        if (wait_status == PROCESS_OK) {
            if (pid != expected_pid || exit_status != expected_status) {
                user_init_failure(label);
            }
            user_init_reap_log(label, pid, exit_status);
            return;
        }
        if (wait_status == PROCESS_NOT_EXITED) {
            if (sched_yield() != SCHEDULER_OK) {
                user_init_failure(label);
            }
            continue;
        }
        user_init_failure(label);
    }
}

/*
 * ITEM-653: two children of one proof can exit in either order.  The test
 * image must assert each child's own pid/status pair, not the order the
 * children happen to be linked in the exited list.  Every expected status is
 * still exact; only the order assumption is removed.
 */
static void user_init_reap_pair(uint32_t pid_a, uint32_t status_a,
                                const char *label_a, uint32_t pid_b,
                                uint32_t status_b, const char *label_b,
                                const char *failure_label) {
    bool a_reaped = false;
    bool b_reaped = false;

    while (!a_reaped || !b_reaped) {
        uint32_t pid = 0U;
        uint32_t exit_status = 0U;
        const enum process_status wait_status =
            process_wait_kernel(&user_init_parent->pcb, &pid, &exit_status,
                                user_init_hhdm);

        if (wait_status == PROCESS_OK) {
            if (!a_reaped && pid == pid_a && exit_status == status_a) {
                user_init_reap_log(label_a, pid, exit_status);
                a_reaped = true;
            } else if (!b_reaped && pid == pid_b && exit_status == status_b) {
                user_init_reap_log(label_b, pid, exit_status);
                b_reaped = true;
            } else {
                user_init_failure(failure_label);
            }
            continue;
        }
        if (wait_status == PROCESS_NOT_EXITED) {
            if (sched_yield() != SCHEDULER_OK) {
                user_init_failure(failure_label);
            }
            continue;
        }
        user_init_failure(failure_label);
    }
}

/*
 * ITEM-626: run the native external programs against the real rootfs and the
 * writable RAM mount.  Each program is a static ELF64 image from /bin; the
 * parent spawns it through the production loader and reaps a real exit status.
 * The check at the end reads the result back through the boot VFS, so a program
 * that only printed the expected text cannot pass.
 */
static struct kernel_process *user_util_child;

static const char *const user_util_env[] = {
    "HOME=/tmp", "PATH=/bin", NULL
};

static void user_util_run(const char *label, const char *path,
                          const char *const *argv, uint32_t expected_status)
{
    enum user_process_status status;

    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[USER-UTIL] spawn ");
        serial_write(label);
        serial_write(" path=");
        serial_write(path);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    status = user_process_spawn(&user_util_child, &user_init_parent->pcb, path,
                                argv, user_util_env, user_init_hhdm);
    if (status != USER_PROCESS_OK) {
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-UTIL] failed: ");
            serial_write(label);
            serial_write(" ");
            serial_write(user_process_status_string(status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        user_init_failure("utility spawn");
    }
    user_init_reap_child(user_util_child->pcb.pid, expected_status, label);
}

static bool user_util_file_equals(const char *path, const char *expected,
                                  size_t expected_length)
{
    struct vfs_context context;
    struct vfs_file *file = NULL;
    char buffer[256];
    size_t total = 0U;
    const struct vfs *fs = kernel_vfs_boot_instance();

    if (fs == NULL || expected_length > sizeof(buffer)) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    if (vfs_context_init(&context, (struct vfs *)fs) != VFS_OK ||
        vfs_open(&context, path, VFS_OPEN_READ, &file) != VFS_OK) {
        return false;
    }
    while (total < expected_length) {
        size_t done = 0U;

        if (vfs_read(file, buffer + total, expected_length - total, &done) !=
                VFS_OK ||
            done == 0U) {
            break;
        }
        total += done;
    }
    (void)vfs_close(file);
    return total == expected_length &&
           memcmp(buffer, expected, expected_length) == 0;
}

static bool user_util_path_missing(const char *path)
{
    struct vfs_context context;
    struct vfs_stat stat;
    const struct vfs *fs = kernel_vfs_boot_instance();

    if (fs == NULL) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    if (vfs_context_init(&context, (struct vfs *)fs) != VFS_OK) {
        return false;
    }
    return vfs_stat(&context, path, &stat) == VFS_NOT_FOUND;
}

static bool user_util_file_size_equals(const char *path, size_t expected)
{
    struct vfs_context context;
    struct vfs_stat stat;
    const struct vfs *fs = kernel_vfs_boot_instance();

    if (fs == NULL) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    if (vfs_context_init(&context, (struct vfs *)fs) != VFS_OK ||
        vfs_stat(&context, path, &stat) != VFS_OK) {
        return false;
    }
    return (size_t)stat.size == expected;
}

static bool user_util_file_contains(const char *path, const char *needle)
{
    struct vfs_context context;
    struct vfs_file *file = NULL;
    char buffer[256];
    size_t total = 0U;
    const size_t needle_length = strlen(needle);
    const struct vfs *fs = kernel_vfs_boot_instance();

    if (fs == NULL || needle_length == 0U) {
        return false;
    }
    memset(&context, 0, sizeof(context));
    if (vfs_context_init(&context, (struct vfs *)fs) != VFS_OK ||
        vfs_open(&context, path, VFS_OPEN_READ, &file) != VFS_OK) {
        return false;
    }
    while (total + 1U < sizeof(buffer)) {
        size_t done = 0U;

        if (vfs_read(file, buffer + total, sizeof(buffer) - 1U - total,
                     &done) != VFS_OK ||
            done == 0U) {
            break;
        }
        total += done;
    }
    (void)vfs_close(file);
    buffer[total] = '\0';
    if (needle_length > total) {
        return false;
    }
    {
        size_t offset;

        for (offset = 0U; offset + needle_length <= total; ++offset) {
            if (memcmp(buffer + offset, needle, needle_length) == 0) {
                return true;
            }
        }
    }
    return false;
}

static struct kernel_process *user_pipe_spawn(
    const char *label, const char *path, const char *const *argv,
    struct hfoc_pipe *pipe, bool writer)
{
    struct kernel_process *child = NULL;
    uint32_t descriptor = 0U;
    enum user_process_status status;

    status = user_process_spawn(&child, &user_init_parent->pcb, path, argv,
                                user_util_env, user_init_hhdm);
    if (status != USER_PROCESS_OK) {
        user_init_failure("pipe spawn");
    }
    if (pipe_bind_fd(child, pipe, writer, &descriptor) != PIPE_OK ||
        descriptor != 3U) {
        user_init_failure("pipe bind");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();

        serial_write("[USER-PIPE] spawn ");
        serial_write(label);
        serial_write(" pid=");
        serial_write_u64(child->pcb.pid);
        serial_write(" fd=");
        serial_write_u64(descriptor);
        serial_write(" end=");
        serial_write(writer ? "write" : "read");
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    return child;
}

static void user_pipe_proof(void)
{
    static const char *const produce_argv[] = {
        "pipetest", "produce", "8192", NULL
    };
    static const char *const consume_argv[] = {
        "pipetest", "consume", "8192", NULL
    };
    static const char *const dup_argv[] = {
        "pipetest", "dup", NULL
    };
    static const char *const consume_dup_argv[] = {
        "pipetest", "consume", "1024", NULL
    };
    static const char *const early_argv[] = {
        "pipetest", "early", NULL
    };
    static const char *const produce_early_argv[] = {
        "pipetest", "produce", "20000", NULL
    };
    static const char *const consume_block_argv[] = {
        "pipetest", "consume", "1", NULL
    };
    struct hfoc_pipe *pipe = NULL;
    struct kernel_process *producer;
    struct kernel_process *consumer;

    serial_write("[USER-PIPE] start\n");

    /* 1. 8192 bytes through a 4096-byte ring: the writer must block and the
       reader must drain while the parent waits. */
    if (pipe_create(&pipe) != PIPE_OK) {
        user_init_failure("pipe create");
    }
    producer = user_pipe_spawn("produce", "/bin/pipetest", produce_argv, pipe,
                               true);
    consumer = user_pipe_spawn("consume", "/bin/pipetest", consume_argv, pipe,
                               false);
    pipe_end_release(&pipe->read_end);
    pipe_end_release(&pipe->write_end);
    /* Both ends exit 0; the writer closes first, so reap it first. */
    user_init_reap_pair(producer->pcb.pid, 0U, "pipe-produce",
                        consumer->pcb.pid, 0U, "pipe-consume",
                        "pipe-stream-pair");
    serial_write("[USER-PIPE] stream OK bytes=8192\n");

    /* 2. dup2 keeps the same open end alive after the original fd closes. */
    if (pipe_create(&pipe) != PIPE_OK) {
        user_init_failure("dup pipe create");
    }
    producer = user_pipe_spawn("dup", "/bin/pipetest", dup_argv, pipe, true);
    consumer = user_pipe_spawn("consume-dup", "/bin/pipetest",
                               consume_dup_argv, pipe, false);
    pipe_end_release(&pipe->read_end);
    pipe_end_release(&pipe->write_end);
    user_init_reap_pair(producer->pcb.pid, 0U, "pipe-dup",
                        consumer->pcb.pid, 0U, "pipe-consume-dup",
                        "pipe-dup-pair");
    serial_write("[USER-PIPE] dup OK bytes=1024\n");

    /* 3. a reader that leaves early makes the blocked writer's next call
       EPIPE (42), without stalling the parent. */
    if (pipe_create(&pipe) != PIPE_OK) {
        user_init_failure("early pipe create");
    }
    producer = user_pipe_spawn("produce-early", "/bin/pipetest",
                               produce_early_argv, pipe, true);
    consumer = user_pipe_spawn("early", "/bin/pipetest", early_argv, pipe,
                               false);
    pipe_end_release(&pipe->read_end);
    pipe_end_release(&pipe->write_end);
    user_init_reap_pair(consumer->pcb.pid, 0U, "pipe-early",
                        producer->pcb.pid, 42U, "pipe-broken",
                        "pipe-early-pair");
    serial_write("[USER-PIPE] broken-pipe status=42\n");

    /* 4. a reader blocked on its own queue can be cancelled and reaped. */
    if (pipe_create(&pipe) != PIPE_OK) {
        user_init_failure("cancel pipe create");
    }
    consumer = user_pipe_spawn("consume-block", "/bin/pipetest",
                               consume_block_argv, pipe, false);
    pipe_end_release(&pipe->read_end);
    if (sched_yield() != SCHEDULER_OK) {
        user_init_failure("pipe cancel yield");
    }
    if (process_cancel_kernel(consumer, 130U) != PROCESS_OK) {
        user_init_failure("pipe cancel");
    }
    user_init_reap_child(consumer->pcb.pid, 130U, "pipe-cancel");
    pipe_end_release(&pipe->write_end);
    serial_write("[USER-PIPE] cancel OK status=130\n");
    serial_write("[USER-PIPE] proof OK\n");
}

static void user_util_proof(void)
{
    static const char *const mkdir_argv[] = {
        "mkdir", "/tmp/utildir", NULL
    };
    static const char *const ls_long_argv[] = {
        "ls", "-l", "/etc", NULL
    };
    static const char *const cp_argv[] = {
        "cp", "/etc/motd", "/tmp/utildir/motd", NULL
    };
    static const char *const cat_argv[] = {
        "cat", "/tmp/utildir/motd", NULL
    };
    static const char *const wc_argv[] = {
        "wc", "/tmp/utildir/motd", NULL
    };
    static const char *const grep_argv[] = {
        "grep", "OS", "/tmp/utildir/motd", NULL
    };
    static const char *const mv_argv[] = {
        "mv", "/tmp/utildir/motd", "/tmp/utildir/renamed", NULL
    };
    static const char *const cp_second_argv[] = {
        "cp", "/tmp/utildir/renamed", "/tmp/utildir/second", NULL
    };
    static const char *const rm_renamed_argv[] = {
        "rm", "/tmp/utildir/renamed", NULL
    };
    static const char *const rm_second_argv[] = {
        "rm", "/tmp/utildir/second", NULL
    };
    static const char *const cp_readonly_argv[] = {
        "cp", "/etc/motd", "/etc/denied", NULL
    };
    static const char *const cat_missing_argv[] = {
        "cat", "/missing", NULL
    };
    static const char *const echo_argv[] = {
        "echo", "hello-native", NULL
    };
    static const char *const true_argv[] = { "true", NULL };
    static const char *const false_argv[] = { "false", NULL };
    static const char *const pwd_argv[] = { "pwd", NULL };
    static const char *const ls_argv[] = { "ls", "/etc", NULL };
    static const char *const hfoc_argv[] = { "hfoc", NULL };
    static const char *const hfoc_env_argv[] = { "hfoc", "env", NULL };
    static const char *const hfoc_exec_argv[] = {
        "hfoc", "exec", "/bin/echo", "replaced-ok", NULL
    };
    static const char *const hfoc_exec_missing_argv[] = {
        "hfoc", "exec", "/bin/missing", NULL
    };
    static const char *const hfoc_exec_bad_argv[] = {
        "hfoc", "exec", "/etc/motd", NULL
    };
    static const char *const mkdir_unicode_argv[] = {
        "mkdir", "/tmp/" "\xD1\x8E\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB4",
        NULL
    };
    static const char *const cp_unicode_argv[] = {
        "cp", "/etc/motd",
        "/tmp/" "\xD1\x8E\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB4" "/"
        "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB",
        NULL
    };
    static const char *const ls_unicode_argv[] = {
        "ls", "/tmp/" "\xD1\x8E\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB4",
        NULL
    };
    static const char *const cat_unicode_argv[] = {
        "cat",
        "/tmp/" "\xD1\x8E\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB4" "/"
        "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB",
        NULL
    };
    static const char *const cp_enospc_argv[] = {
        "cp", "/etc/big", "/tmp/big", NULL
    };
    static const char *const rm_big_argv[] = {
        "rm", "-f", "/tmp/big", NULL
    };

    serial_write("[USER-UTIL] start\n");
    if (!user_util_file_equals("/etc/motd", "HFOC OS\n", 8U)) {
        user_init_failure("rootfs motd");
    }
    serial_write("[USER-UTIL] rootfs motd OK\n");
    user_util_run("ls-long", "/bin/ls", ls_long_argv, 0U);
    user_util_run("mkdir", "/bin/mkdir", mkdir_argv, 0U);
    user_util_run("cp", "/bin/cp", cp_argv, 0U);
    user_util_run("cat", "/bin/cat", cat_argv, 0U);
    user_util_run("wc", "/bin/wc", wc_argv, 0U);
    user_util_run("grep", "/bin/grep", grep_argv, 0U);
    user_util_run("mv", "/bin/mv", mv_argv, 0U);
    user_util_run("cp-second", "/bin/cp", cp_second_argv, 0U);
    user_util_run("rm-renamed", "/bin/rm", rm_renamed_argv, 0U);
    if (!user_util_file_equals("/tmp/utildir/second", "HFOC OS\n", 8U) ||
        !user_util_path_missing("/tmp/utildir/renamed")) {
        user_init_failure("utility file roundtrip");
    }
    serial_write("[USER-UTIL] roundtrip OK bytes=8\n");
    user_util_run("rm-second", "/bin/rm", rm_second_argv, 0U);
    user_util_run("cp-readonly", "/bin/cp", cp_readonly_argv, 1U);
    user_util_run("cat-missing", "/bin/cat", cat_missing_argv, 1U);
    user_util_run("echo", "/bin/echo", echo_argv, 0U);
    user_util_run("true", "/bin/true", true_argv, 0U);
    user_util_run("false", "/bin/false", false_argv, 1U);
    user_util_run("pwd", "/bin/pwd", pwd_argv, 0U);
    user_util_run("ls", "/bin/ls", ls_argv, 0U);
    user_util_run("hfoc", "/bin/hfoc", hfoc_argv, 0U);
    user_util_run("hfoc-env", "/bin/hfoc", hfoc_env_argv, 0U);
    user_util_run("hfoc-exec", "/bin/hfoc", hfoc_exec_argv, 0U);
    user_util_run("hfoc-exec-missing", "/bin/hfoc", hfoc_exec_missing_argv,
                  127U);
    user_util_run("hfoc-exec-bad", "/bin/hfoc", hfoc_exec_bad_argv, 126U);
    user_util_run("mkdir-unicode", "/bin/mkdir", mkdir_unicode_argv, 0U);
    user_util_run("cp-unicode", "/bin/cp", cp_unicode_argv, 0U);
    user_util_run("ls-unicode", "/bin/ls", ls_unicode_argv, 0U);
    user_util_run("cat-unicode", "/bin/cat", cat_unicode_argv, 0U);
    if (!user_util_file_equals(
            "/tmp/" "\xD1\x8E\xD0\xBD\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB4" "/"
            "\xD1\x84\xD0\xB0\xD0\xB9\xD0\xBB",
            "HFOC OS\n", 8U)) {
        user_init_failure("unicode file roundtrip");
    }
    serial_write("[USER-UTIL] unicode OK\n");
    user_util_run("cp-enospc", "/bin/cp", cp_enospc_argv, 1U);
    serial_write("[USER-UTIL] enospc refused\n");
    user_util_run("rm-big", "/bin/rm", rm_big_argv, 0U);
    serial_write("[USER-UTIL] proof OK\n");
}

/*
 * ITEM-628: the shell itself is a real /bin/sh ELF64 process.  The script and
 * the -c command below run PIPE/SPAWN/WAIT and real /bin utilities; the result
 * is read back through the boot VFS, so printing the expected text is not
 * enough to pass.
 */
static void user_sh_proof(void)
{
    static const char *const script_argv[] = {
        "sh", "/etc/sh-test.sh", NULL
    };
    static const char *const inline_argv[] = {
        "sh", "-c", "echo INLINE | wc -c > /tmp/sh-inline", NULL
    };
    static const char *const script632_argv[] = {
        "sh", "-c",
        "sh /etc/sh-632-red.sh alpha beta > /tmp/sh-632-out; "
        "echo status=$? >> /tmp/sh-632-out", NULL
    };
    static const char *const syntax632_argv[] = {
        "sh", "-c", "if true; then echo bad", NULL
    };
    static const char *const clean_argv[] = {
        "sh", "-c",
        "rm -f /tmp/sh-nf-err /tmp/sh-err-sep /tmp/sh-out-only "
        "/tmp/sh-sep-status", NULL
    };
    static const char expected632[] =
        "hello alpha\nhello beta\nif-ok\ni=a\ni=c\nf:value\nret=3\n"
        "args=2\nfirst=alpha\nsub=inner\n/tmp\nouter=outer\nsubx=inner\n"
        "outer2=outer\nend\ntrap-ok\nstatus=0\n";

    serial_write("[USER-SH] start\n");
    user_util_run("sh-script", "/bin/sh", script_argv, 0U);
    if (!user_util_file_equals("/tmp/sh-out",
                               "quoted\na b\nor-ok\nand-ok\nstatus=1\n",
                               33U)) {
        serial_write("[USER-SH] failed: script output\n");
        user_init_failure("sh script output");
    }
    if (!user_util_file_equals("/tmp/sh-pipe", "PIPE\n", 5U)) {
        serial_write("[USER-SH] failed: pipeline output\n");
        user_init_failure("sh pipeline output");
    }
    if (!user_util_file_contains("/tmp/sh-err", "no such")) {
        serial_write("[USER-SH] failed: 2>&1 output\n");
        user_init_failure("sh stderr redirection");
    }
    if (!user_util_file_equals("/tmp/sh-out-only", "", 0U) ||
        !user_util_file_size_equals("/tmp/sh-out-only", 0U)) {
        serial_write("[USER-SH] failed: 2> separation stdout\n");
        user_init_failure("sh stderr separation stdout");
    }
    if (!user_util_file_contains("/tmp/sh-err-sep", "no such")) {
        serial_write("[USER-SH] failed: 2> separation stderr\n");
        user_init_failure("sh stderr separation stderr");
    }
    if (!user_util_file_equals("/tmp/sh-sep-status", "sep=1\nnf=127\n",
                               13U)) {
        serial_write("[USER-SH] failed: 2> separation status\n");
        user_init_failure("sh stderr separation status");
    }
    if (!user_util_file_contains("/tmp/sh-err", "command not found")) {
        serial_write("[USER-SH] failed: not-found split stderr\n");
        user_init_failure("sh not-found split stderr");
    }
    if (!user_util_file_contains("/tmp/sh-nf-err", "command not found")) {
        serial_write("[USER-SH] failed: not-found merged stream\n");
        user_init_failure("sh not-found merged stream");
    }
    user_util_run("sh-clean", "/bin/sh", clean_argv, 0U);
    serial_write("[USER-SH] script OK\n");
    user_util_run("sh-inline", "/bin/sh", inline_argv, 0U);
    if (!user_util_file_equals("/tmp/sh-inline", "7\n", 2U)) {
        serial_write("[USER-SH] failed: inline pipeline\n");
        user_init_failure("sh inline pipeline");
    }
    serial_write("[USER-SH] inline OK\n");
    user_util_run("sh-632", "/bin/sh", script632_argv, 0U);
    if (!user_util_file_equals("/tmp/sh-632-out", expected632,
                               sizeof(expected632) - 1U)) {
        serial_write("[USER-SH] failed: script language output\n");
        user_init_failure("sh script language output");
    }
    serial_write("[USER-SH] scripts OK\n");
    user_util_run("sh-632-syntax", "/bin/sh", syntax632_argv, 2U);
    serial_write("[USER-SH] syntax OK\n");
    serial_write("[USER-SH] proof OK\n");
}

static void user_init_parent_entry(void) {
    uint32_t reaped = 0U;

    serial_write("[USER-INIT] parent wait start\n");
    while (reaped < 1U) {
        uint32_t pid = 0U;
        uint32_t exit_status = 0U;
        const enum process_status wait_status =
            process_wait_kernel(&user_init_parent->pcb,
                                &pid, &exit_status,
                                user_init_hhdm);
        if (wait_status == PROCESS_OK) {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[USER-INIT] parent reaped pid=");
                serial_write_u64(pid);
                serial_write(" status=");
                serial_write_u64(exit_status);
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            ++reaped;
        } else if (wait_status == PROCESS_NOT_EXITED) {
            const enum scheduler_status yield_status = sched_yield();
            if (yield_status != SCHEDULER_OK) {
                user_init_failure("parent yield");
            }
        } else {
            {
                const uint64_t serial_flags = serial_sequence_begin();
                serial_write("[USER-INIT] parent wait failed: ");
                serial_write(process_status_string(wait_status));
                serial_write("\n");
                serial_sequence_end(serial_flags);
            }
            user_init_failure("wait");
        }
    }

    serial_write("[USER-INIT] parent reaped all\n");
    {
        const enum user_process_status spawn_status =
            user_process_spawn(&user_init_elf_child, &user_init_parent->pcb,
                               "/bin/hello", user_init_elf_argv, NULL,
                               user_init_hhdm);
        if (spawn_status != USER_PROCESS_OK) {
            user_init_failure(user_process_status_string(spawn_status));
        }
        user_init_elf_pid = user_init_elf_child->pcb.pid;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[USER-EXEC] spawn pid=");
            serial_write_u64(user_init_elf_pid);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        serial_write("[USER-EXEC] spawned\n");
    }
    {
        bool reaped = false;

        while (!reaped) {
            uint32_t pid = 0U;
            uint32_t exit_status = 0U;
            const enum process_status wait_status =
                process_wait_kernel(&user_init_parent->pcb, &pid,
                                    &exit_status, user_init_hhdm);

            if (wait_status == PROCESS_OK) {
                if (pid != user_init_elf_pid) {
                    user_init_failure("elf reap pid");
                }
                {
                    const uint64_t serial_flags = serial_sequence_begin();
                    serial_write("[USER-EXEC] reaped pid=");
                    serial_write_u64(pid);
                    serial_write(" status=");
                    serial_write_u64(exit_status);
                    serial_write("\n");
                    serial_sequence_end(serial_flags);
                }
                reaped = true;
            } else if (wait_status == PROCESS_NOT_EXITED) {
                if (sched_yield() != SCHEDULER_OK) {
                    user_init_failure("elf parent yield");
                }
            } else {
                user_init_failure("elf wait");
            }
        }
    }
    {
        const enum user_process_status crash_status =
            user_process_spawn(&user_init_crash_child, &user_init_parent->pcb,
                               "/bin/crash", NULL, NULL, user_init_hhdm);
        if (crash_status != USER_PROCESS_OK) {
            user_init_failure(user_process_status_string(crash_status));
        }
        user_init_crash_pid = user_init_crash_child->pcb.pid;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[USER-EXEC] crash spawn pid=");
            serial_write_u64(user_init_crash_pid);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }
    {
        bool reaped = false;

        while (!reaped) {
            uint32_t pid = 0U;
            uint32_t exit_status = 0U;
            const enum process_status wait_status =
                process_wait_kernel(&user_init_parent->pcb, &pid,
                                    &exit_status, user_init_hhdm);

            if (wait_status == PROCESS_OK) {
                if (pid != user_init_crash_pid || exit_status != 142U) {
                    user_init_failure("crash reap status");
                }
                {
                    const uint64_t serial_flags = serial_sequence_begin();
                    serial_write("[USER-EXEC] crash reaped pid=");
                    serial_write_u64(pid);
                    serial_write(" status=");
                    serial_write_u64(exit_status);
                    serial_write("\n");
                    serial_sequence_end(serial_flags);
                }
                reaped = true;
            } else if (wait_status == PROCESS_NOT_EXITED) {
                if (sched_yield() != SCHEDULER_OK) {
                    user_init_failure("crash wait yield");
                }
            } else {
                user_init_failure("crash wait");
            }
        }
    }
    user_init_audit_observability_proof();
    {
        const uint64_t free_before = pmm_free_page_count();
        const uint32_t sched_before = sched_count();
        struct kernel_process *child = NULL;
        struct kernel_process *pair_a = NULL;
        struct kernel_process *pair_b = NULL;
        struct kernel_process *spinner = NULL;
        uint32_t pid = 0U;

        /* 1. exec replaces the image in place, PID and stdout preserved. */
        if (user_process_spawn(&child, &user_init_parent->pcb, "/bin/exec-ok",
                               user_init_elf_argv, NULL,
                               user_init_hhdm) != USER_PROCESS_OK) {
            user_init_failure("exec spawn");
        }
        pid = child->pcb.pid;
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] exec spawn pid=");
            serial_write_u64(pid);
            serial_write(" path=/bin/exec-ok\n");
            serial_sequence_end(serial_flags);
        }
        user_init_reap_child(pid, 0U, "exec");

        /* 2. a failed exec leaves the old process alive and running. */
        if (user_process_spawn(&child, &user_init_parent->pcb,
                               "/bin/exec-missing", NULL, NULL,
                               user_init_hhdm) != USER_PROCESS_OK) {
            user_init_failure("missing exec spawn");
        }
        pid = child->pcb.pid;
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] missing spawn pid=");
            serial_write_u64(pid);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        user_init_reap_child(pid, 0U, "missing");

        /* 3. two user processes are runnable in independent address spaces. */
        if (user_process_spawn(&pair_a, &user_init_parent->pcb, "/bin/hello",
                               user_init_elf_argv, NULL,
                               user_init_hhdm) != USER_PROCESS_OK ||
            user_process_spawn(&pair_b, &user_init_parent->pcb, "/bin/hello",
                               user_init_elf_argv, NULL,
                               user_init_hhdm) != USER_PROCESS_OK) {
            user_init_failure("pair spawn");
        }
        if (pair_a->pcb.pid == pair_b->pcb.pid) {
            user_init_failure("pair pid");
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] pair spawn pid=");
            serial_write_u64(pair_a->pcb.pid);
            serial_write(",");
            serial_write_u64(pair_b->pcb.pid);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        if (sched_yield() != SCHEDULER_OK) {
            user_init_failure("pair yield");
        }
        user_init_reap_child(pair_a->pcb.pid, 0U, "pair-a");
        user_init_reap_child(pair_b->pcb.pid, 0U, "pair-b");

        /* 4. cancel a queued child, then reap its documented status. */
        if (user_process_spawn(&spinner, &user_init_parent->pcb, "/bin/spin",
                               NULL, NULL,
                               user_init_hhdm) != USER_PROCESS_OK) {
            user_init_failure("cancel spawn");
        }
        pid = spinner->pcb.pid;
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] cancel spawn pid=");
            serial_write_u64(pid);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        if (sched_yield() != SCHEDULER_OK) {
            user_init_failure("cancel yield");
        }
        if (process_cancel_kernel(spinner, 130U) != PROCESS_OK) {
            user_init_failure("cancel");
        }
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] cancel pid=");
            serial_write_u64(pid);
            serial_write(" result=PROCESS_OK\n");
            serial_sequence_end(serial_flags);
        }
        user_init_reap_child(pid, 130U, "cancel");

        /* 5. every PID, user page, page table and run-queue slot came back. */
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-EXEC] leak check free=");
            serial_write_u64(free_before);
            serial_write("->");
            serial_write_u64(pmm_free_page_count());
            serial_write(" sched=");
            serial_write_u64(sched_before);
            serial_write("->");
            serial_write_u64(sched_count());
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        if (pmm_free_page_count() != free_before ||
            sched_count() != sched_before) {
            user_init_failure("user lifecycle leak");
        }
    }
    {
        const uint64_t free_before = pmm_free_page_count();
        const uint32_t sched_before = sched_count();

        user_util_proof();
        user_sh_proof();
        user_pipe_proof();
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write("[USER-UTIL] leak check free=");
            serial_write_u64(free_before);
            serial_write("->");
            serial_write_u64(pmm_free_page_count());
            serial_write(" sched=");
            serial_write_u64(sched_before);
            serial_write("->");
            serial_write_u64(sched_count());
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        if (pmm_free_page_count() != free_before ||
            sched_count() != sched_before) {
            user_init_failure("utility lifecycle leak");
        }
    }
    const enum syscall_abi_status restore_status =
        syscall_msr_restore();
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[USER-INIT] MSR restore=");
        serial_write(syscall_abi_status_string(restore_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    if (restore_status != SYSCALL_ABI_OK) {
        user_init_failure("MSR restore");
    }
    serial_write("[USER-INIT] proof OK\n");
}

static void user_init_process_entry(void) {
    struct scheduler_task *task = NULL;
    struct kernel_process *process;
    struct user_iret_frame entry_frame;

    if (sched_current(&task) != SCHEDULER_OK || task == NULL ||
        task->process_owner == NULL) {
        user_init_failure("process owner");
    }
    process = (struct kernel_process *)task->process_owner;
    if (vmm_activate_address_space(&process->address_space) != VMM_OK) {
        user_init_failure("address space");
    }
    if (user_iret_frame_init(
            &entry_frame,
            VMM_USER_IMAGE_BASE,
            USER_INIT_STACK_TOP,
            0x2U,
            USER_CS_SELECTOR,
            USER_SS_SELECTOR) != USER_ENTRY_OK) {
        user_init_failure("entry frame");
    }
    serial_write("[USER-INIT] user entry prepared\n");
    user_enter_iretq(&entry_frame);
}

enum user_init_status user_init_qemu_test_init(uint64_t hhdm_offset) {
    static uint8_t measured_image[USER_INIT_IMAGE_MAX_SIZE];
    static uint8_t tampered_image[USER_INIT_IMAGE_MAX_SIZE];
    static struct image_integrity_payload image_payload;
    static struct image_integrity_gate image_gate;
    static enum process_status process_status;
    static enum syscall_abi_status msr_status;
    static struct syscall_origin child_origin;
    static struct syscall_origin stale_origin;
    static struct kernel_process stale_process;
    static struct process_control_block denied_process;
    static struct syscall_origin denied_origin;
    static uint32_t stale_pid = 0U;
    static uint32_t stale_reused = 0U;
    static uint32_t denied_pid = 0U;

    serial_write("[USER-INIT] proof init\n");
    user_init_hhdm = hhdm_offset;
    user_init_zero_bytes(&stale_process, sizeof(stale_process));

    user_init_zero_bytes(&image_gate, sizeof(image_gate));
    if (!user_init_image_copy(tampered_image, USER_INIT_IMAGE_MAX_SIZE)) {
        user_init_failure("tampered image copy");
    }
    if (user_init_image_size() != 0U) {
        tampered_image[0] ^= 0x01U;
    }
    image_payload.name = "initramfs";
    image_payload.data = tampered_image;
    image_payload.data_length = user_init_image_size();
    if (image_integrity_verify(&image_gate,
                               user_init_integrity_manifest(),
                               &image_payload, 1U) !=
        IMAGE_INTEGRITY_HASH_MISMATCH) {
        user_init_failure("image integrity mismatch");
    }
    serial_write("[IMAGE-INTEGRITY] mismatch rejected before init launch\n");

    if (!user_init_image_copy(measured_image, USER_INIT_IMAGE_MAX_SIZE)) {
        user_init_failure("measured image copy");
    }
    image_payload.name = "initramfs";
    image_payload.data = measured_image;
    image_payload.data_length = user_init_image_size();
    if (image_integrity_verify(&image_gate,
                               user_init_integrity_manifest(),
                               &image_payload, 1U) != IMAGE_INTEGRITY_OK) {
        user_init_failure("image integrity verify");
    }
    serial_write("[IMAGE-INTEGRITY] verified initramfs before init launch\n");

    if (sched_init() != SCHEDULER_OK) {
        user_init_failure("sched init");
    }
    msr_status = syscall_msr_prepare();
    if (msr_status != SYSCALL_ABI_OK ||
        syscall_msr_verify() != SYSCALL_ABI_OK) {
        user_init_failure("MSR verify");
    }
    serial_write("[USER-INIT] MSR verify OK\n");

    process_status = process_create_kernel(
        &user_init_parent, NULL,
        user_init_parent_entry, hhdm_offset);
    if (process_status != PROCESS_OK) {
        user_init_failure("parent create");
    }
    process_status = process_create_user_init(
        &user_init_child, &user_init_parent->pcb,
        user_init_process_entry, hhdm_offset);
    if (process_status != PROCESS_OK) {
        user_init_failure("child create");
    }
    user_init_child_pid = user_init_child->pcb.pid;
    if (!capability_table_has(
            &user_init_child->pcb.capability_table,
            CAPABILITY_TYPE_SYSCALL_WRITE) ||
        !capability_table_has(
            &user_init_child->pcb.capability_table,
            CAPABILITY_TYPE_SYSCALL_YIELD) ||
        !capability_table_has(
            &user_init_child->pcb.capability_table,
            CAPABILITY_TYPE_SYSCALL_EXIT) ||
        !capability_table_has(
            &user_init_child->pcb.capability_table,
            CAPABILITY_TYPE_SYSCALL_FILE) ||
        capability_table_open_count(
            &user_init_child->pcb.capability_table) != 4U) {
        user_init_failure("capability publication");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[CAPABILITY] child syscall capabilities OK pid=");
        serial_write_u64(user_init_child->pcb.pid);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    const enum syscall_origin_status capture_status =
        syscall_origin_capture(&user_init_child->pcb, &child_origin);
    if (capture_status != SYSCALL_ORIGIN_OK) {
        user_init_failure("security capture");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SECURITY] capture OK pid=");
        serial_write_u64(user_init_child->pcb.pid);
        serial_write(" domain=");
        serial_write_u64(child_origin.domain);
        serial_write(" generation=");
        serial_write_u64(child_origin.generation);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    struct syscall_origin cross_origin = child_origin;
    cross_origin.domain = PROCESS_SECURITY_DOMAIN_KERNEL;
    if (syscall_handle_checked(&cross_origin, SYSCALL_WRITE,
                               0U, 0U, 0U) != SYSCALL_RET_EPERM) {
        user_init_failure("cross-domain result");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SECURITY] cross-domain rejected pid=");
        serial_write_u64(user_init_child->pcb.pid);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    process_status = process_pcb_init_domain(
        &stale_process.pcb, &user_init_parent->pcb, &stale_pid,
        PROCESS_SECURITY_DOMAIN_USER);
    if (process_status != PROCESS_OK) {
        user_init_failure("stale create");
    }
    if (syscall_origin_capture(&stale_process.pcb, &stale_origin) !=
        SYSCALL_ORIGIN_OK) {
        user_init_failure("stale capture");
    }
    if (process_exit_kernel(&stale_process, 0U) != PROCESS_OK) {
        user_init_failure("stale exit");
    }
    if (syscall_handle_checked(&stale_origin, SYSCALL_WRITE,
                               0U, 0U, 0U) != SYSCALL_RET_EPERM) {
        user_init_failure("stale-origin result");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SECURITY] stale-origin rejected pid=");
        serial_write_u64(stale_pid);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    user_init_zero_bytes(&denied_process, sizeof(denied_process));
    process_status = process_pcb_init_domain(
        &denied_process, &user_init_parent->pcb, &denied_pid,
        PROCESS_SECURITY_DOMAIN_USER);
    if (process_status != PROCESS_OK) {
        user_init_failure("denied create");
    }
    if (syscall_origin_capture(&denied_process, &denied_origin) !=
        SYSCALL_ORIGIN_OK) {
        user_init_failure("denied capture");
    }
    if (syscall_handle_checked(&denied_origin, SYSCALL_WRITE,
                               0U, 0U, 0U) != SYSCALL_RET_EPERM) {
        user_init_failure("denied result");
    }
    if (capability_table_open_count(&denied_process.capability_table) != 0U ||
        capability_table_has(&denied_process.capability_table,
                             CAPABILITY_TYPE_SYSCALL_WRITE) ||
        capability_table_has(&denied_process.capability_table,
                             CAPABILITY_TYPE_SYSCALL_YIELD) ||
        capability_table_has(&denied_process.capability_table,
                             CAPABILITY_TYPE_SYSCALL_EXIT)) {
        user_init_failure("denied state");
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[CAPABILITY] write denied pid=");
        serial_write_u64(denied_pid);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[CAPABILITY] denied state unchanged pid=");
        serial_write_u64(denied_pid);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    if (process_pcb_destroy(&denied_process) != PROCESS_OK) {
        user_init_failure("denied cleanup");
    }
    if (process_pcb_destroy(&stale_process.pcb) != PROCESS_OK) {
        user_init_failure("stale cleanup");
    }
    if (process_pid_alloc(&stale_reused) != PROCESS_OK ||
        stale_reused != stale_pid) {
        user_init_failure("stale PID reuse");
    }
    if (process_pid_free(stale_reused) != PROCESS_OK) {
        user_init_failure("stale PID free");
    }

    process_status = process_spawn_kernel(
        user_init_parent, hhdm_offset);
    if (process_status != PROCESS_OK) {
        user_init_failure("parent spawn");
    }
    process_status = process_spawn_kernel(
        user_init_child, hhdm_offset);
    if (process_status != PROCESS_OK) {
        user_init_failure("child spawn");
    }
    return USER_INIT_OK;
}

#endif

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_user_init_qemu_test_only_marker;
#endif
