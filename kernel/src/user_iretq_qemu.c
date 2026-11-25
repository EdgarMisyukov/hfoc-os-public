#ifdef HFOC_USER_IRETQ_QEMU_TEST

#include "user.h"

#include "arch/x86_64/tss.h"
#include "pmm.h"
#include "serial.h"
#include "vmm.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define USER_IRETQ_TEST_MAGIC UINT32_C(0x600D)
#define USER_IRETQ_TEST_CODE_FLAGS \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define USER_IRETQ_TEST_STACK_FLAGS \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX)
#define USER_IRETQ_TEST_STACK_TOP (VMM_USER_STACK_BASE + 0x1000ULL)

__asm__(
".text\n"
".global user_ring3_test_code_start\n"
".type user_ring3_test_code_start, @function\n"
"user_ring3_test_code_start:\n"
"    mov %cs, %ax\n"
"    mov %ax, user_ring3_test_marker_cs(%rip)\n"
"    movl $0x600D, user_ring3_test_marker_magic(%rip)\n"
"    ud2\n"
"user_ring3_test_marker_cs:\n"
"    .word 0\n"
"user_ring3_test_marker_magic:\n"
"    .long 0\n"
".global user_ring3_test_code_end\n"
"user_ring3_test_code_end:\n"
"    ud2\n"
);

extern char user_ring3_test_code_start[];
extern char user_ring3_test_marker_cs[];
extern char user_ring3_test_marker_magic[];
extern char user_ring3_test_code_end[];

static uint64_t kernel_return_stack[512]
    __attribute__((aligned(16)));
static uint64_t user_iretq_hhdm;
static uint64_t user_iretq_code_phys;
static uint64_t user_iretq_stack_phys;
static uint64_t user_iretq_previous_rsp0;
static struct address_space user_iretq_test_as;
static struct address_space user_iretq_saved_as;
static bool user_iretq_address_space_created;

__attribute__((noreturn))
static void user_iretq_kernel_return(void) {
    const size_t code_start = (uintptr_t)user_ring3_test_code_start;
    const size_t marker_cs_offset =
        (uintptr_t)user_ring3_test_marker_cs - code_start;
    const size_t marker_magic_offset =
        (uintptr_t)user_ring3_test_marker_magic - code_start;
    const volatile uint16_t *cs =
        (const volatile uint16_t *)(uintptr_t)(
            user_iretq_hhdm + user_iretq_code_phys + marker_cs_offset);
    const volatile uint32_t *magic =
        (const volatile uint32_t *)(uintptr_t)(
            user_iretq_hhdm + user_iretq_code_phys + marker_magic_offset);
    const bool marker_ok =
        *cs == USER_CS_SELECTOR && *magic == USER_IRETQ_TEST_MAGIC;

    if (!marker_ok) {
        serial_write("[USER] failed: marker mismatch\n");
    } else {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[USER] returned; cs=");
            serial_write_hex(*cs);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        serial_write("[USER] proof OK\n");
    }

    vmm_activate_address_space(&user_iretq_saved_as);
    if (user_iretq_address_space_created) {
        (void)vmm_destroy_address_space(
            &user_iretq_test_as, user_iretq_hhdm);
        user_iretq_address_space_created = false;
    }
    tss_set_rsp0(user_iretq_previous_rsp0);
    if (user_iretq_code_phys != 0U) {
        (void)pmm_free_page(user_iretq_code_phys);
        user_iretq_code_phys = 0U;
    }
    if (user_iretq_stack_phys != 0U) {
        (void)pmm_free_page(user_iretq_stack_phys);
        user_iretq_stack_phys = 0U;
    }
    serial_write("[USER] cleanup OK\n");

    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

void user_iretq_qemu_trap_complete(void) {
    user_iretq_kernel_return();
}

enum user_entry_status user_iretq_qemu_test_init(uint64_t hhdm_offset) {
    const struct address_space *saved = vmm_current_address_space();
    struct user_iret_frame entry_frame;
    size_t code_size;
    uint8_t *code_page;
    uint8_t *stack_page;
    uint64_t code_pages[1];
    uint64_t stack_pages[1];

    serial_write("[USER] proof init\n");
    if (saved == NULL) {
        serial_write("[USER] failed: no address space\n");
        return USER_ENTRY_NULL;
    }
    user_iretq_saved_as = *saved;
    user_iretq_hhdm = hhdm_offset;

    if (vmm_create_address_space(&user_iretq_test_as, hhdm_offset) !=
        VMM_OK) {
        serial_write("[USER] failed: address space\n");
        return USER_ENTRY_NULL;
    }
    user_iretq_address_space_created = true;

    user_iretq_code_phys = pmm_alloc_page();
    user_iretq_stack_phys = pmm_alloc_page();
    if (user_iretq_code_phys == 0U || user_iretq_stack_phys == 0U) {
        serial_write("[USER] failed: page allocation\n");
        return USER_ENTRY_NULL;
    }

    code_page = (uint8_t *)(uintptr_t)(
        hhdm_offset + user_iretq_code_phys);
    stack_page = (uint8_t *)(uintptr_t)(
        hhdm_offset + user_iretq_stack_phys);
    memset(stack_page, 0, 4096U);

    code_size = (uintptr_t)user_ring3_test_code_end -
                (uintptr_t)user_ring3_test_code_start;
    memcpy(code_page, user_ring3_test_code_start, code_size);
    if (code_size < 4096U) {
        memset(code_page + code_size, 0, 4096U - code_size);
    }

    code_pages[0] = user_iretq_code_phys;
    stack_pages[0] = user_iretq_stack_phys;
    if (vmm_map_user_pages_in(
            &user_iretq_test_as,
            VMM_USER_IMAGE_BASE,
            code_pages,
            1U,
            USER_IRETQ_TEST_CODE_FLAGS,
            hhdm_offset) != VMM_OK ||
        vmm_map_user_pages_in(
            &user_iretq_test_as,
            VMM_USER_STACK_BASE,
            stack_pages,
            1U,
            USER_IRETQ_TEST_STACK_FLAGS,
            hhdm_offset) != VMM_OK) {
        serial_write("[USER] failed: mapping\n");
        return USER_ENTRY_NULL;
    }

    if (user_iret_frame_init(
            &entry_frame,
            VMM_USER_IMAGE_BASE,
            USER_IRETQ_TEST_STACK_TOP,
            0x2U,
            USER_CS_SELECTOR,
            USER_SS_SELECTOR) != USER_ENTRY_OK) {
        serial_write("[USER] failed: entry frame\n");
        return USER_ENTRY_NULL;
    }

    user_iretq_previous_rsp0 = tss_rsp0();
    tss_set_rsp0((uint64_t)(uintptr_t)&kernel_return_stack[512]);
    vmm_activate_address_space(&user_iretq_test_as);
    user_enter_iretq(&entry_frame);
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_user_iretq_qemu_test_only_marker;
#endif
