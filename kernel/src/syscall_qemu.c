#ifdef HFOC_SYSCALL_QEMU_TEST

#include "syscall.h"

#include "arch/x86_64/tss.h"
#include "pmm.h"
#include "serial.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define SYSCALL_QEMU_TEST_MAGIC UINT32_C(0x5CA11)
#define SYSCALL_QEMU_TEST_CODE_FLAGS \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define SYSCALL_QEMU_TEST_STACK_FLAGS \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX)
#define SYSCALL_QEMU_TEST_STACK_TOP (VMM_USER_STACK_BASE + 0x1000ULL)

__asm__(
".text\n"
".global user_syscall_test_code_start\n"
".type user_syscall_test_code_start, @function\n"
"user_syscall_test_code_start:\n"
"    mov %cs, %ax\n"
"    mov %ax, user_syscall_test_marker_cs(%rip)\n"
"    movl $0x5CA11, user_syscall_test_marker_magic(%rip)\n"
"    movl $0, %eax\n"
"    movl $1, %edi\n"
"    leaq user_syscall_test_text(%rip), %rsi\n"
"    movl $7, %edx\n"
"    syscall\n"
"    movq %rax, user_syscall_test_marker_write(%rip)\n"
"    movl $1, %eax\n"
"    xorl %edi, %edi\n"
"    xorl %esi, %esi\n"
"    xorl %edx, %edx\n"
"    syscall\n"
"    movq %rax, user_syscall_test_marker_yield(%rip)\n"
"    movl $2, %eax\n"
"    xorl %edi, %edi\n"
"    xorl %esi, %esi\n"
"    xorl %edx, %edx\n"
"    syscall\n"
"    movq %rax, user_syscall_test_marker_exit(%rip)\n"
"    ud2\n"
"user_syscall_test_marker_cs:\n"
"    .word 0\n"
"user_syscall_test_marker_magic:\n"
"    .long 0\n"
"user_syscall_test_marker_write:\n"
"    .quad 0\n"
"user_syscall_test_marker_yield:\n"
"    .quad 0\n"
"user_syscall_test_marker_exit:\n"
"    .quad 0\n"
"user_syscall_test_text:\n"
"    .ascii \"SYSCALL\\n\"\n"
".global user_syscall_test_code_end\n"
"user_syscall_test_code_end:\n"
);

extern char user_syscall_test_code_start[];
extern char user_syscall_test_marker_cs[];
extern char user_syscall_test_marker_magic[];
extern char user_syscall_test_marker_write[];
extern char user_syscall_test_marker_yield[];
extern char user_syscall_test_marker_exit[];
extern char user_syscall_test_code_end[];

static uint64_t syscall_qemu_hhdm;
static uint64_t syscall_qemu_code_phys;
static uint64_t syscall_qemu_stack_phys;
static uint64_t syscall_kernel_return_stack[512]
    __attribute__((aligned(16)));
static uint64_t syscall_qemu_previous_rsp0;
static struct address_space syscall_qemu_test_as;
static struct address_space syscall_qemu_saved_as;
static bool syscall_qemu_address_space_created;

static uint64_t marker_u64(const char *marker, size_t code_start) {
    return *(const volatile uint64_t *)(uintptr_t)(
        syscall_qemu_hhdm + syscall_qemu_code_phys +
        ((size_t)(uintptr_t)marker - code_start));
}

__attribute__((noreturn))
static void syscall_qemu_complete(void) {
    const size_t code_start = (uintptr_t)user_syscall_test_code_start;
    const volatile uint16_t *cs =
        (const volatile uint16_t *)(uintptr_t)(
            syscall_qemu_hhdm + syscall_qemu_code_phys +
            ((size_t)(uintptr_t)user_syscall_test_marker_cs - code_start));
    const volatile uint32_t *magic =
        (const volatile uint32_t *)(uintptr_t)(
            syscall_qemu_hhdm + syscall_qemu_code_phys +
            ((size_t)(uintptr_t)user_syscall_test_marker_magic - code_start));
    const uint16_t cs_value = *cs;
    const uint32_t magic_value = *magic;
    const uint64_t write_result = marker_u64(
        user_syscall_test_marker_write, code_start);
    const uint64_t yield_result = marker_u64(
        user_syscall_test_marker_yield, code_start);
    const uint64_t exit_result = marker_u64(
        user_syscall_test_marker_exit, code_start);
    const bool marker_ok =
        cs_value == USER_CS_SELECTOR &&
        magic_value == SYSCALL_QEMU_TEST_MAGIC &&
        write_result == 7U &&
        yield_result == SYSCALL_RET_OK &&
        exit_result == SYSCALL_RET_OK;

    vmm_activate_address_space(&syscall_qemu_saved_as);
    if (syscall_qemu_address_space_created) {
        (void)vmm_destroy_address_space(
            &syscall_qemu_test_as, syscall_qemu_hhdm);
        syscall_qemu_address_space_created = false;
    }
    tss_set_rsp0(syscall_qemu_previous_rsp0);
    if (syscall_qemu_code_phys != 0U) {
        (void)pmm_free_page(syscall_qemu_code_phys);
        syscall_qemu_code_phys = 0U;
    }
    if (syscall_qemu_stack_phys != 0U) {
        (void)pmm_free_page(syscall_qemu_stack_phys);
        syscall_qemu_stack_phys = 0U;
    }

    const enum syscall_abi_status restore_status =
        syscall_msr_restore();
    if (!marker_ok) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SYSCALL] failed: marker mismatch cs=");
            serial_write_hex(cs_value);
            serial_write(" magic=");
            serial_write_hex(magic_value);
            serial_write(" write=");
            serial_write_u64(write_result);
            serial_write(" yield=");
            serial_write_u64(yield_result);
            serial_write(" exit=");
            serial_write_u64(exit_result);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    } else if (restore_status != SYSCALL_ABI_OK) {
        serial_write("[SYSCALL] failed: MSR restore\n");
    } else {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SYSCALL] returned; cs=");
            serial_write_hex(cs_value);
            serial_write(" write=");
            serial_write_u64(write_result);
            serial_write(" yield=");
            serial_write_u64(yield_result);
            serial_write(" exit=");
            serial_write_u64(exit_result);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        serial_write("[SYSCALL] proof OK\n");
    }
    serial_write("[SYSCALL] cleanup OK\n");

    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

void syscall_qemu_trap_complete(void) {
    syscall_qemu_complete();
}

enum syscall_abi_status syscall_qemu_test_init(uint64_t hhdm_offset) {
    const struct address_space *saved = vmm_current_address_space();
    struct user_iret_frame entry_frame;
    size_t code_size;
    uint8_t *code_page;
    uint8_t *stack_page;
    uint64_t code_pages[1];
    uint64_t stack_pages[1];
    enum syscall_abi_status msr_status;

    serial_write("[SYSCALL] proof init\n");
    if (saved == NULL) {
        serial_write("[SYSCALL] failed: no address space\n");
        return SYSCALL_ABI_NOT_PUBLISHED;
    }
    syscall_qemu_saved_as = *saved;
    syscall_qemu_hhdm = hhdm_offset;

    msr_status = syscall_msr_prepare();
    if (msr_status != SYSCALL_ABI_OK ||
        syscall_msr_verify() != SYSCALL_ABI_OK) {
        serial_write("[SYSCALL] failed: MSR verify\n");
        return msr_status;
    }
    serial_write("[SYSCALL] MSR verify OK\n");
    uint32_t star_low;
    uint32_t star_high;
    __asm__ volatile ("rdmsr"
                      : "=a"(star_low), "=d"(star_high)
                      : "c"(IA32_STAR_MSR));
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[SYSCALL] STAR=");
        serial_write_hex(((uint64_t)star_high << 32U) | star_low);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }

    if (vmm_create_address_space(&syscall_qemu_test_as, hhdm_offset) !=
        VMM_OK) {
        serial_write("[SYSCALL] failed: address space\n");
        return SYSCALL_ABI_NOT_PUBLISHED;
    }
    syscall_qemu_address_space_created = true;

    syscall_qemu_code_phys = pmm_alloc_page();
    syscall_qemu_stack_phys = pmm_alloc_page();
    if (syscall_qemu_code_phys == 0U || syscall_qemu_stack_phys == 0U) {
        serial_write("[SYSCALL] failed: page allocation\n");
        return SYSCALL_ABI_NOT_PUBLISHED;
    }

    code_page = (uint8_t *)(uintptr_t)(
        hhdm_offset + syscall_qemu_code_phys);
    stack_page = (uint8_t *)(uintptr_t)(
        hhdm_offset + syscall_qemu_stack_phys);
    memset(stack_page, 0, 4096U);
    code_size = (uintptr_t)user_syscall_test_code_end -
                (uintptr_t)user_syscall_test_code_start;
    memcpy(code_page, user_syscall_test_code_start, code_size);
    if (code_size < 4096U) {
        memset(code_page + code_size, 0, 4096U - code_size);
    }

    code_pages[0] = syscall_qemu_code_phys;
    stack_pages[0] = syscall_qemu_stack_phys;
    if (vmm_map_user_pages_in(
            &syscall_qemu_test_as,
            VMM_USER_IMAGE_BASE,
            code_pages,
            1U,
            SYSCALL_QEMU_TEST_CODE_FLAGS,
            hhdm_offset) != VMM_OK ||
        vmm_map_user_pages_in(
            &syscall_qemu_test_as,
            VMM_USER_STACK_BASE,
            stack_pages,
            1U,
            SYSCALL_QEMU_TEST_STACK_FLAGS,
            hhdm_offset) != VMM_OK) {
        serial_write("[SYSCALL] failed: mapping\n");
        return SYSCALL_ABI_NOT_PUBLISHED;
    }

    if (user_iret_frame_init(
            &entry_frame,
            VMM_USER_IMAGE_BASE,
            SYSCALL_QEMU_TEST_STACK_TOP,
            0x2U,
            USER_CS_SELECTOR,
            USER_SS_SELECTOR) != USER_ENTRY_OK) {
        serial_write("[SYSCALL] failed: entry frame\n");
        return SYSCALL_ABI_NOT_PUBLISHED;
    }

    syscall_qemu_previous_rsp0 = tss_rsp0();
    tss_set_rsp0(
        (uint64_t)(uintptr_t)&syscall_kernel_return_stack[512]);
    vmm_activate_address_space(&syscall_qemu_test_as);
    user_enter_iretq(&entry_frame);
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_syscall_qemu_test_only_marker;
#endif
