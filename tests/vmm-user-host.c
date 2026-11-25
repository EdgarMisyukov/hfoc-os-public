#include "vmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOCK_ROOT         0x1000ULL
#define MOCK_ALLOC_BASE   0x5000ULL
#define MOCK_ALLOC_COUNT  8U
#define TEST_HHDM         0xFFFF800000000000ULL
#define PAGE_SIZE         4096ULL

#define USER_FLAGS \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | VMM_FLAG_NX)

static uint64_t mock_pml4[512];
static uint64_t mock_pages[MOCK_ALLOC_COUNT][512];

static uint64_t alloc_pool[MOCK_ALLOC_COUNT];
static int alloc_count;
static int alloc_index;
static bool alloc_used[MOCK_ALLOC_COUNT];
static int free_call_count;
static uint64_t freed_pages[MOCK_ALLOC_COUNT];

static uint64_t mock_cr3;
static int invlpg_count;
static uint64_t last_invlpg_addr;
static int zeroed_call_count;
static int zeroed_failure;
static int failures;

static uint64_t user_pml4_idx;
static uint64_t user_pdpt_idx;
static uint64_t user_pd_idx;
static uint64_t user_pt_idx;

#define TEST(name)  do { printf("  %s: ", name); } while (0)
#define PASS()      do { puts("PASS"); } while (0)
#define FAIL(msg)   do { puts("FAIL: " msg); ++failures; return; } while (0)
#define CHECK(c, m) do { if (!(c)) { FAIL(m); } } while (0)

uint64_t mock_read_cr3(void) {
    return mock_cr3;
}

void mock_write_cr3(uint64_t cr3) {
    mock_cr3 = cr3;
}

void mock_invlpg(uint64_t virtual_address) {
    ++invlpg_count;
    last_invlpg_addr = virtual_address;
}

void *vmm_test_phys_to_virt(uint64_t physical) {
    if (physical == MOCK_ROOT) {
        return mock_pml4;
    }
    if (physical >= MOCK_ALLOC_BASE &&
        physical < MOCK_ALLOC_BASE + MOCK_ALLOC_COUNT * PAGE_SIZE) {
        const uint64_t index = (physical - MOCK_ALLOC_BASE) / PAGE_SIZE;
        if ((physical & 0xFFFULL) == 0U) {
            return mock_pages[index];
        }
    }
    fprintf(stderr, "vmm_test_phys_to_virt: unknown physical 0x%lx\n",
            (unsigned long)physical);
    abort();
}

uint64_t pmm_alloc_page(void) {
    if (alloc_index >= alloc_count) {
        return 0U;
    }
    const uint64_t page = alloc_pool[alloc_index];
    alloc_used[alloc_index] = true;
    ++alloc_index;
    return page;
}

bool pmm_free_page(uint64_t physical_address) {
    for (int index = 0; index < alloc_count; ++index) {
        if (alloc_pool[index] == physical_address && alloc_used[index]) {
            alloc_used[index] = false;
            freed_pages[free_call_count++] = physical_address;
            return true;
        }
    }
    return false;
}

void vmm_test_note_page_zeroed(uint64_t physical) {
    ++zeroed_call_count;
    const unsigned char *page = vmm_test_phys_to_virt(physical);
    for (size_t index = 0U; index < PAGE_SIZE; ++index) {
        if (page[index] != 0U) {
            zeroed_failure = 1;
            return;
        }
    }
}

static void init_indices(void) {
    const uint64_t address = VMM_USER_IMAGE_BASE;
    user_pml4_idx = (address >> 39) & 0x1FFU;
    user_pdpt_idx = (address >> 30) & 0x1FFU;
    user_pd_idx   = (address >> 21) & 0x1FFU;
    user_pt_idx   = (address >> 12) & 0x1FFU;
}

static void reset_state(void) {
    memset(mock_pml4, 0, sizeof(mock_pml4));
    for (int index = 256; index < 512; ++index) {
        mock_pml4[index] = 0x9000000000000000ULL + (uint64_t)index;
    }
    memset(mock_pages, 0xFF, sizeof(mock_pages));
    memset(alloc_pool, 0, sizeof(alloc_pool));
    alloc_count = 0;
    alloc_index = 0;
    memset(alloc_used, 0, sizeof(alloc_used));
    free_call_count = 0;
    memset(freed_pages, 0, sizeof(freed_pages));
    mock_cr3 = MOCK_ROOT;
    invlpg_count = 0;
    last_invlpg_addr = 0;
    zeroed_call_count = 0;
    zeroed_failure = 0;
}

static void set_alloc_pool(int count) {
    alloc_count = count;
    for (int index = 0; index < count; ++index) {
        alloc_pool[index] = MOCK_ALLOC_BASE + (uint64_t)index * PAGE_SIZE;
    }
}

static uint64_t *address_space_pml4(const struct address_space *as) {
    return (uint64_t *)vmm_test_phys_to_virt(as->pml4_phys);
}

static void test_valid_ranges(void) {
    TEST("valid fixed user ranges");
    reset_state();
    CHECK(vmm_validate_user_range(VMM_USER_IMAGE_BASE, 1U) == VMM_OK,
          "image base page valid");
    CHECK(vmm_validate_user_range(VMM_USER_STACK_BASE, 1U) == VMM_OK,
          "stack base page valid");
    CHECK(vmm_validate_user_range(VMM_USER_STACK_TOP - PAGE_SIZE, 1U) ==
              VMM_OK,
          "last user page valid");
    CHECK(vmm_validate_user_range(
              VMM_USER_IMAGE_BASE,
              (VMM_USER_STACK_TOP - VMM_USER_IMAGE_BASE) / PAGE_SIZE) ==
              VMM_OK,
          "full user range valid");
    PASS();
}

static void test_invalid_ranges(void) {
    TEST("invalid canonical and layout ranges");
    reset_state();
    CHECK(vmm_validate_user_range(VMM_USER_IMAGE_BASE + 1U, 1U) ==
              VMM_BAD_ALIGNMENT,
          "unaligned base rejected");
    CHECK(vmm_validate_user_range(VMM_USER_IMAGE_BASE, 0U) == VMM_BAD_RANGE,
          "zero page count rejected");
    CHECK(vmm_validate_user_range(VMM_USER_IMAGE_BASE - PAGE_SIZE, 1U) ==
              VMM_BAD_RANGE,
          "below image base rejected");
    CHECK(vmm_validate_user_range(VMM_USER_STACK_TOP - PAGE_SIZE, 2U) ==
              VMM_BAD_RANGE,
          "crossing stack top rejected");
    CHECK(vmm_validate_user_range(0xFFFF800000000000ULL, 1U) == VMM_BAD_RANGE,
          "upper-half address rejected");
    const uint64_t huge_count = (UINT64_MAX / PAGE_SIZE) + 1U;
    CHECK(vmm_validate_user_range(VMM_USER_IMAGE_BASE, huge_count) ==
              VMM_BAD_RANGE,
          "overflowing page count rejected");
    PASS();
}

static void test_bad_flags_and_pointers(void) {
    TEST("flags, pointers, and alignment validation");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    set_alloc_pool(4);

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    const uint64_t phys = 0x1000000ULL;

    CHECK(vmm_map_user_pages_in(NULL, VMM_USER_IMAGE_BASE, &phys, 1U,
                                USER_FLAGS, TEST_HHDM) ==
              VMM_BAD_ADDRESS_SPACE,
          "null address space rejected");
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, NULL, 1U,
                                USER_FLAGS, TEST_HHDM) ==
              VMM_BAD_OUTPUT_POINTER,
          "null physical array rejected");
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, &phys, 1U,
                                VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE |
                                    VMM_FLAG_NX,
                                TEST_HHDM) == VMM_BAD_FLAGS,
          "missing USER flag rejected");
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, &phys, 1U,
                                VMM_FLAG_USER | VMM_FLAG_WRITABLE |
                                    VMM_FLAG_NX,
                                TEST_HHDM) == VMM_BAD_FLAGS,
          "missing PRESENT flag rejected");
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, &phys, 1U,
                                USER_FLAGS | (1ULL << 58U),
                                TEST_HHDM) == VMM_BAD_FLAGS,
          "unknown flag bit rejected");
    const uint64_t unaligned_phys = 0x1000001ULL;
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, &unaligned_phys,
                                1U, USER_FLAGS, TEST_HHDM) ==
              VMM_BAD_ALIGNMENT,
          "unaligned physical page rejected");
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, &phys, 0U,
                                USER_FLAGS, TEST_HHDM) == VMM_BAD_RANGE,
          "zero-length mapping rejected");

    CHECK(address_space_pml4(&as)[user_pml4_idx] == 0U,
          "no partial PML4E after validation failures");
    CHECK(invlpg_count == 0, "no invlpg after validation failures");
    CHECK(alloc_index == 1, "no page-table allocations after validation failures");
    PASS();
}

static void test_exact_user_mapping(void) {
    TEST("exact user PTE and intermediate flags");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    set_alloc_pool(4);

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    const uint64_t phys[3] = {
        0x1000000ULL,
        0x1001000ULL,
        0x1002000ULL,
    };
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, phys, 3U,
                                USER_FLAGS, TEST_HHDM) == VMM_OK,
          "multi-page user mapping succeeds");

    uint64_t *const as_pml4 = address_space_pml4(&as);
    const uint64_t pml4e = as_pml4[user_pml4_idx];
    CHECK((pml4e & 0x7ULL) == 0x7ULL,
          "PML4E Present|RW|User exact");
    const uint64_t pdpte = mock_pages[1][user_pdpt_idx];
    CHECK((pdpte & 0x7ULL) == 0x7ULL,
          "PDPTE Present|RW|User exact");
    const uint64_t pde = mock_pages[2][user_pd_idx];
    CHECK((pde & 0x7ULL) == 0x7ULL,
          "PDE Present|RW|User exact");
    uint64_t *const pt = mock_pages[3];
    for (uint32_t index = 0U; index < 3U; ++index) {
        CHECK(pt[user_pt_idx + index] == (phys[index] | USER_FLAGS),
              "exact user PTE published");
    }
    CHECK(invlpg_count == 0,
          "mapping in non-current address space does not invlpg");
    PASS();
}

static void test_idempotent_mapping(void) {
    TEST("identical user mapping is idempotent");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    set_alloc_pool(4);

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    const uint64_t phys[2] = { 0x1000000ULL, 0x1001000ULL };
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, phys, 2U,
                                USER_FLAGS, TEST_HHDM) == VMM_OK,
          "first mapping succeeds");
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, phys, 2U,
                                USER_FLAGS, TEST_HHDM) == VMM_OK,
          "identical mapping succeeds");
    uint64_t *const pt = mock_pages[3];
    CHECK(pt[user_pt_idx] == (phys[0] | USER_FLAGS),
          "first PTE unchanged");
    CHECK(pt[user_pt_idx + 1U] == (phys[1] | USER_FLAGS),
          "second PTE unchanged");
    CHECK(invlpg_count == 0, "no invlpg in non-current address space");
    PASS();
}

static void test_conflict_rollback(void) {
    TEST("conflict rolls back earlier user PTEs");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    set_alloc_pool(4);

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");

    const uint64_t conflicting_phys = 0xDEAD0000ULL;
    const uint64_t conflict_address =
        VMM_USER_IMAGE_BASE + 2U * PAGE_SIZE;
    CHECK(vmm_map_user_pages_in(&as, conflict_address, &conflicting_phys,
                                1U, USER_FLAGS, TEST_HHDM) == VMM_OK,
          "seed conflicting page succeeds");

    const uint64_t phys[3] = {
        0x1000000ULL,
        0x1001000ULL,
        0x1002000ULL,
    };
    CHECK(vmm_map_user_pages_in(&as, VMM_USER_IMAGE_BASE, phys, 3U,
                                USER_FLAGS, TEST_HHDM) ==
              VMM_PTE_CONFLICT,
          "later PTE conflict returns VMM_PTE_CONFLICT");

    uint64_t *const pt = mock_pages[3];
    CHECK(pt[user_pt_idx] == 0U, "first rolled-back PTE is clear");
    CHECK(pt[user_pt_idx + 1U] == 0U, "second rolled-back PTE is clear");
    CHECK(pt[user_pt_idx + 2U] == (conflicting_phys | USER_FLAGS),
          "pre-existing conflicting PTE unchanged");
    CHECK(invlpg_count == 0, "no invlpg in non-current address space");
    PASS();
}

static void test_allocation_rollback(void) {
    TEST("table allocation failure rolls back earlier user PTEs");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    set_alloc_pool(4);

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");

    const uint64_t base = VMM_USER_IMAGE_BASE + 511U * PAGE_SIZE;
    const uint64_t phys[2] = { 0x1000000ULL, 0x1001000ULL };
    CHECK(vmm_map_user_pages_in(&as, base, phys, 2U,
                                USER_FLAGS, TEST_HHDM) ==
              VMM_PMM_ALLOC_FAILED,
          "second page-table allocation failure returns VMM_PMM_ALLOC_FAILED");

    uint64_t *const pt = mock_pages[3];
    CHECK(pt[511U] == 0U, "first page PTE rolled back");
    CHECK(pt[0U] == 0U, "no partial second page PTE");
    CHECK(address_space_pml4(&as)[user_pml4_idx] != 0U,
          "successful first-table intermediate entries remain owned");
    CHECK(invlpg_count == 0, "no invlpg in non-current address space");
    PASS();
}

static void test_status_string_all(void) {
    TEST("status strings for all VMM statuses");
    reset_state();
    for (int status = VMM_OK; status <= VMM_BAD_FLAGS; ++status) {
        const char *text = vmm_status_string((enum vmm_status)status);
        CHECK(text != NULL && text[0] != '\0', "status string non-empty");
    }
    PASS();
}

int main(void) {
    init_indices();
    printf("User layout and mapping host test\n");

    test_valid_ranges();
    test_invalid_ranges();
    test_bad_flags_and_pointers();
    test_exact_user_mapping();
    test_idempotent_mapping();
    test_conflict_rollback();
    test_allocation_rollback();
    test_status_string_all();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("\nUser layout and mapping host tests: ALL OK\n");
    return 0;
}
