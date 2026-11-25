/*
 * ITEM-624 host proof for the native file-call surface.
 *
 * The production syscall_file.c runs against a real VFS/ramfs and the real
 * descriptor owner.  Only the address-space check is replaced by a bounded
 * host window; the page walk itself is covered by tests/user-copy-host.c.
 */
#include "syscall_file.h"
#include "descriptor.h"
#include "kernel_vfs.h"
#include "process.h"
#include "syscall.h"
#include "vfs.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static struct vfs host_vfs;
static struct vfs_context host_context;
static struct kernel_process process;
static struct kernel_process other_process;
static uint8_t user_memory[8192];
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static uint64_t user_pointer(size_t offset) {
    return (uint64_t)(uintptr_t)(user_memory + offset);
}

bool user_copy_host_range_ok(uint64_t address, size_t length, bool write) {
    const uint64_t base = (uint64_t)(uintptr_t)user_memory;
    (void)write;
    return address >= base && length <= sizeof(user_memory) &&
           address - base <= sizeof(user_memory) - length;
}

const struct vfs *kernel_vfs_boot_instance(void) { return &host_vfs; }

static struct syscall_origin origin_for(struct kernel_process *target) {
    struct syscall_origin origin;

    memset(&origin, 0, sizeof(origin));
    origin.pcb = &target->pcb;
    origin.domain = target->pcb.security_domain;
    origin.generation = target->pcb.security_generation;
    return origin;
}

static void put_string(size_t offset, const char *text) {
    const size_t length = strlen(text) + 1U;

    memcpy(user_memory + offset, text, length);
}

static uint64_t call(struct syscall_origin *origin, uint64_t number,
                     uint64_t arg0, uint64_t arg1, uint64_t arg2) {
    return syscall_file_handle(origin, number, arg0, arg1, arg2);
}

static void setup(void) {
    struct vfs_context context;

    memset(&process, 0, sizeof(process));
    memset(&other_process, 0, sizeof(other_process));
    memset(user_memory, 0, sizeof(user_memory));
    CHECK(vfs_init(&host_vfs) == VFS_OK, "the test VFS initialises");
    CHECK(vfs_context_init(&context, &host_vfs) == VFS_OK,
          "the test context initialises");
    CHECK(vfs_mkdir(&context, "/tmp") == VFS_OK, "the test /tmp exists");
    CHECK(vfs_context_init(&host_context, &host_vfs) == VFS_OK,
          "the process context initialises");
    process.pcb.published = true;
    process.pcb.security_domain = PROCESS_SECURITY_DOMAIN_USER;
    process.pcb.security_generation = 1U;
    other_process.pcb.published = true;
    other_process.pcb.security_domain = PROCESS_SECURITY_DOMAIN_USER;
    other_process.pcb.security_generation = 1U;
    CHECK(descriptor_table_init(&process.descriptor_table) == DESCRIPTOR_OK,
          "the process descriptor table initialises");
    CHECK(descriptor_table_init(&other_process.descriptor_table) ==
              DESCRIPTOR_OK,
          "the second descriptor table initialises");
}

static void test_open_write_read(void) {
    struct syscall_origin origin = origin_for(&process);
    uint64_t descriptor;
    uint64_t result;

    put_string(0, "/tmp/alpha");
    descriptor = call(&origin, SYSCALL_OPEN, user_pointer(0),
                      SYSCALL_OPEN_CREATE | SYSCALL_OPEN_READ |
                          SYSCALL_OPEN_WRITE,
                      0U);
    CHECK(descriptor < PROCESS_DESCRIPTOR_CAPACITY,
          "open creates a file and returns a descriptor");
    put_string(256, "hello");
    result = call(&origin, SYSCALL_FILE_WRITE, descriptor, user_pointer(256), 5U);
    CHECK(result == 5U, "write stores the five bytes");
    CHECK(call(&origin, SYSCALL_LSEEK, descriptor, 0U, SYSCALL_SEEK_SET) ==
              0U,
          "lseek rewinds the shared description");
    memset(user_memory + 512, 0, 8U);
    result = call(&origin, SYSCALL_READ, descriptor, user_pointer(512), 8U);
    CHECK(result == 5U && memcmp(user_memory + 512, "hello", 5U) == 0,
          "read returns the stored bytes");
    CHECK(call(&origin, SYSCALL_READ, descriptor, 0U, 4U) ==
              SYSCALL_RET_EFAULT,
          "a null read buffer is EFAULT on a valid descriptor");
    CHECK(call(&origin, SYSCALL_STAT, user_pointer(0),
               user_pointer(768), 0U) == SYSCALL_RET_OK,
          "stat reads the file metadata");
    {
        const struct syscall_stat *stat =
            (const struct syscall_stat *)(user_memory + 768);
        CHECK(stat->type == SYSCALL_STAT_FILE && stat->size == 5U,
              "stat reports a five-byte file");
    }
    CHECK(call(&origin, SYSCALL_CLOSE, descriptor, 0U, 0U) == SYSCALL_RET_OK,
          "close releases the descriptor");
}

static void test_read_only_and_errors(void) {
    struct syscall_origin origin = origin_for(&process);
    uint64_t descriptor;
    uint64_t result;

    put_string(0, "/tmp/alpha");
    descriptor = call(&origin, SYSCALL_OPEN, user_pointer(0),
                      SYSCALL_OPEN_READ, 0U);
    CHECK(descriptor < PROCESS_DESCRIPTOR_CAPACITY,
          "a read-only open succeeds");
    put_string(256, "x");
    CHECK(call(&origin, SYSCALL_FILE_WRITE, descriptor, user_pointer(256), 1U) ==
              SYSCALL_RET_EBADF,
          "write on a read-only descriptor is refused");
    CHECK(call(&origin, SYSCALL_CLOSE, descriptor, 0U, 0U) == SYSCALL_RET_OK,
          "the read-only descriptor closes");

    put_string(0, "/tmp/missing");
    CHECK(call(&origin, SYSCALL_OPEN, user_pointer(0), SYSCALL_OPEN_READ,
               0U) == SYSCALL_RET_ENOENT,
          "a missing file is ENOENT");
    CHECK(call(&origin, SYSCALL_READ, 63U, user_pointer(512), 4U) ==
              SYSCALL_RET_EBADF,
          "a stale descriptor is EBADF");
    CHECK(call(&origin, SYSCALL_FILE_WRITE, 0U, 0U, 4U) ==
              SYSCALL_RET_EBADF,
          "a null user buffer is EFAULT");
    CHECK(call(&origin, SYSCALL_FILE_WRITE, 0U, user_pointer(0), 1U) ==
              SYSCALL_RET_EBADF,
          "write on a closed descriptor is EBADF");
    result = call(&origin, SYSCALL_OPEN, user_pointer(0), 0x100U, 0U);
    CHECK(result == SYSCALL_RET_EINVAL, "unknown open flags are refused");
}

static void test_directories_and_names(void) {
    struct syscall_origin origin = origin_for(&process);
    char cwd[64];
    uint64_t result;
    uint64_t index;

    put_string(0, "/tmp/dir");
    CHECK(call(&origin, SYSCALL_MKDIR, user_pointer(0), 0U, 0U) ==
              SYSCALL_RET_OK,
          "mkdir creates a directory");
    put_string(128, "/tmp/dir");
    CHECK(call(&origin, SYSCALL_CHDIR, user_pointer(128), 0U, 0U) ==
              SYSCALL_RET_OK,
          "chdir enters the directory");
    memset(cwd, 0, sizeof(cwd));
    CHECK(call(&origin, SYSCALL_GETCWD, user_pointer(512),
               sizeof(cwd), 0U) == SYSCALL_RET_OK &&
              strcmp((char *)user_memory + 512, "/tmp/dir") == 0,
          "getcwd reports the process cwd");
    put_string(0, "/tmp/alpha");
    put_string(128, "/tmp/dir/beta");
    CHECK(call(&origin, SYSCALL_RENAME, user_pointer(0),
               user_pointer(128), 0U) == SYSCALL_RET_OK,
          "rename moves the file");
    CHECK(call(&origin, SYSCALL_STAT, user_pointer(128),
               user_pointer(768), 0U) == SYSCALL_RET_OK,
          "the renamed file is visible");
    CHECK(call(&origin, SYSCALL_UNLINK, user_pointer(128), 0U, 0U) ==
              SYSCALL_RET_OK,
          "unlink removes the file");
    put_string(0, "/tmp");
    result = call(&origin, SYSCALL_GETDENTS, user_pointer(0), 0U,
                  user_pointer(768));
    CHECK(result == SYSCALL_RET_OK,
          "getdents lists the first directory entry");
    index = 0U;
    while (call(&origin, SYSCALL_GETDENTS, user_pointer(0), index,
                user_pointer(768)) == SYSCALL_RET_OK) {
        ++index;
    }
    CHECK(index != 0U, "getdents enumerates a non-empty directory");
    put_string(0, "/");
    CHECK(call(&origin, SYSCALL_CHDIR, user_pointer(0), 0U, 0U) ==
              SYSCALL_RET_OK,
          "chdir leaves the directory before it is removed");
    put_string(0, "/tmp/dir");
    CHECK(call(&origin, SYSCALL_UNLINK, user_pointer(0), 0U, 0U) ==
              SYSCALL_RET_OK,
          "an empty directory unlinks");
    CHECK(call(&origin, SYSCALL_CHDIR, user_pointer(0), 0U, 0U) ==
              SYSCALL_RET_ENOENT,
          "chdir to a removed directory is ENOENT");
}

static void test_cross_process_descriptor(void) {
    struct syscall_origin origin = origin_for(&process);
    struct syscall_origin other = origin_for(&other_process);
    uint64_t other_descriptor;

    put_string(0, "/tmp/other");
    other_descriptor = call(&other, SYSCALL_OPEN, user_pointer(0),
                            SYSCALL_OPEN_CREATE | SYSCALL_OPEN_READ, 0U);
    CHECK(other_descriptor < PROCESS_DESCRIPTOR_CAPACITY,
          "the second process opens its own file");
    CHECK(call(&origin, SYSCALL_READ, other_descriptor,
               user_pointer(512), 1U) == SYSCALL_RET_EBADF,
          "a descriptor from another process is not visible");
    CHECK(call(&other, SYSCALL_CLOSE, other_descriptor, 0U, 0U) ==
              SYSCALL_RET_OK,
          "the second process closes its file");
}

int main(void) {
    setup();
    test_open_write_read();
    test_read_only_and_errors();
    test_directories_and_names();
    test_cross_process_descriptor();
    if (failures != 0) {
        fprintf(stderr, "syscall file host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("syscall file host test: ALL OK\n");
    return 0;
}
