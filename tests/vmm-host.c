#include "vmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Synthetic page tables (4096 bytes each = uint64_t[512]) ---- */
static uint64_t mock_pml4[512];
static uint64_t mock_pdpt[512];
static uint64_t mock_pd[512];
static uint64_t mock_pt[512];

/* ---- PMM alloc pool: 3 pages for PDPT/PD/PT ---- */
static uint64_t mock_alloc0[512];
static uint64_t mock_alloc1[512];
static uint64_t mock_alloc2[512];
static uint64_t mock_alloc3[512];

/* ---- Known physical addresses ---- */
#define PHYS_PML4   0x1000ULL
#define PHYS_PDPT   0x2000ULL
#define PHYS_PD     0x3000ULL
#define PHYS_PT     0x4000ULL
#define PHYS_ALLOC0 0x5000ULL
#define PHYS_ALLOC1 0x6000ULL
#define PHYS_ALLOC2 0x7000ULL
#define PHYS_ALLOC3 0x8000ULL

/* ---- Test constants ---- */
#define TEST_PHYS  0xFEE00000ULL
#define TEST_HHDM  0xFFFF800000000000ULL
#define TEST_VIRT  0x1000ULL
#define TEST_PHYS2 0x12345000ULL

/* ---- Computed indices ---- */
static uint64_t pml4_idx, pdpt_idx, pd_idx, pt_idx;
static uint64_t virt_pml4_idx, virt_pdpt_idx, virt_pd_idx, virt_pt_idx;

/* ---- invlpg tracking ---- */
static int      invlpg_count;
static uint64_t last_invlpg_addr;

/* ---- pmm_alloc_page control ---- */
static uint64_t alloc_pool[4];
static int      alloc_count;
static int      alloc_index;
static bool     alloc_used[4];
static int      free_call_count;
static uint64_t freed_pages[4];
static uint64_t mock_cr3;

/* ---- zeroing callback tracking ---- */
static int      zeroed_call_count;
static int      zeroed_failure;   /* non-zero if any byte not zero */

/* ---- External mocks (linkage required by vmm.c) ---- */

uint64_t mock_read_cr3(void) {
    return mock_cr3;
}

void mock_write_cr3(uint64_t cr3) {
    mock_cr3 = cr3;
}

void mock_invlpg(uint64_t virtual_address) {
    invlpg_count++;
    last_invlpg_addr = virtual_address;
}

void *vmm_test_phys_to_virt(uint64_t physical) {
    switch (physical) {
    case PHYS_PML4:   return mock_pml4;
    case PHYS_PDPT:   return mock_pdpt;
    case PHYS_PD:     return mock_pd;
    case PHYS_PT:     return mock_pt;
    case PHYS_ALLOC0: return mock_alloc0;
    case PHYS_ALLOC1: return mock_alloc1;
    case PHYS_ALLOC2: return mock_alloc2;
    case PHYS_ALLOC3: return mock_alloc3;
    default:
        fprintf(stderr, "vmm_test_phys_to_virt: unknown physical 0x%lx\n",
                (unsigned long)physical);
        abort();
    }
}

uint64_t pmm_alloc_page(void) {
    if (alloc_index >= alloc_count) return 0;
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
    zeroed_call_count++;
    const unsigned char *p = vmm_test_phys_to_virt(physical);
    for (size_t i = 0; i < 4096; ++i) {
        if (p[i] != 0) {
            zeroed_failure = 1;
            return;
        }
    }
}

/* ---- Test infrastructure ---- */

static int failures = 0;

#define TEST(name)  do { printf("  %s: ", name); } while (0)
#define PASS()      do { puts("PASS"); } while (0)
#define FAIL(msg)   do { puts("FAIL: " msg); ++failures; return; } while (0)
#define CHECK(c, m) do { if (!(c)) { FAIL(m); } } while (0)

static void init_indices(void) {
    uint64_t vaddr = TEST_PHYS + TEST_HHDM;
    pml4_idx = (vaddr >> 39) & 0x1FF;
    pdpt_idx = (vaddr >> 30) & 0x1FF;
    pd_idx   = (vaddr >> 21) & 0x1FF;
    pt_idx   = (vaddr >> 12) & 0x1FF;

    vaddr = TEST_VIRT;
    virt_pml4_idx = (vaddr >> 39) & 0x1FF;
    virt_pdpt_idx = (vaddr >> 30) & 0x1FF;
    virt_pd_idx   = (vaddr >> 21) & 0x1FF;
    virt_pt_idx   = (vaddr >> 12) & 0x1FF;
}

static void reset_state(void) {
    memset(mock_pml4, 0, sizeof(mock_pml4));
    memset(mock_pdpt, 0, sizeof(mock_pdpt));
    memset(mock_pd,   0, sizeof(mock_pd));
    memset(mock_pt,   0, sizeof(mock_pt));
    /* Pre-fill alloc pages with 0xFF for zeroing detection */
    memset(mock_alloc0, 0xFF, sizeof(mock_alloc0));
    memset(mock_alloc1, 0xFF, sizeof(mock_alloc1));
    memset(mock_alloc2, 0xFF, sizeof(mock_alloc2));
    memset(mock_alloc3, 0xFF, sizeof(mock_alloc3));
    invlpg_count = 0;
    last_invlpg_addr = 0;
    alloc_count = 0;
    alloc_index = 0;
    memset(alloc_used, 0, sizeof(alloc_used));
    free_call_count = 0;
    memset(freed_pages, 0, sizeof(freed_pages));
    mock_cr3 = PHYS_PML4;
    zeroed_call_count = 0;
    zeroed_failure = 0;
}

/* Helper: wire up all tables so the walk reaches PT */
static void wire_all_tables(void) {
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    mock_pdpt[pdpt_idx] = PHYS_PD   | 0x3ULL;
    mock_pd[pd_idx]     = PHYS_PT   | 0x3ULL;
}

/* Helper: save entry snapshots before a call */
static uint64_t saved_pml4e, saved_pdpte, saved_pde, saved_pte;
static void save_entries(void) {
    saved_pml4e = mock_pml4[pml4_idx];
    saved_pdpte = mock_pdpt[pdpt_idx];
    saved_pde   = mock_pd[pd_idx];
    saved_pte   = mock_pt[pt_idx];
}
#define CHECK_ENTRIES_UNCHANGED() do { \
    CHECK(mock_pml4[pml4_idx] == saved_pml4e, "PML4E unchanged"); \
    CHECK(mock_pdpt[pdpt_idx] == saved_pdpte, "PDPTE unchanged"); \
    CHECK(mock_pd[pd_idx]     == saved_pde,   "PDE unchanged"); \
    CHECK(mock_pt[pt_idx]     == saved_pte,   "PTE unchanged"); \
} while (0)

/* ================================================================
 * 18 Test Cases
 * ================================================================ */

static void test_bad_output_pointer(void) {
    TEST("BAD_OUTPUT_POINTER");
    reset_state();
    save_entries();
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, NULL);
    CHECK(s == VMM_BAD_OUTPUT_POINTER, "expected status 6");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_bad_alignment(void) {
    TEST("BAD_ALIGNMENT");
    reset_state();
    save_entries();
    volatile void *out = (void *)0xDEAD;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS | 1, TEST_HHDM, &out);
    CHECK(s == VMM_BAD_ALIGNMENT, "expected status 1");
    CHECK(out == (void *)0xDEAD, "output unchanged");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_hhdm_overflow(void) {
    TEST("HHDM_OVERFLOW");
    reset_state();
    save_entries();
    volatile void *out = (void *)0xDEAD;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, UINT64_MAX, &out);
    CHECK(s == VMM_HHDM_OVERFLOW, "expected status 2");
    CHECK(out == (void *)0xDEAD, "output unchanged");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_ok_new_mapping(void) {
    TEST("OK -- new mapping");
    reset_state();
    /* PML4E and PDPTE and PDE present, PT entry empty */
    wire_all_tables();
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_count = 3;

    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK");
    CHECK(out != NULL, "output set");
    uintptr_t expected = (uintptr_t)(TEST_PHYS + TEST_HHDM);
    CHECK((uintptr_t)out == expected, "*out_virtual correct");
    CHECK(invlpg_count == 1, "invlpg called once");
    CHECK(last_invlpg_addr == expected, "invlpg for correct address");
    PASS();
}

static void test_intermediate_flags(void) {
    TEST("Intermediate flags (0x3)");
    reset_state();
    /* Only PML4E present — need PDPT, PD, PT.
       PDPT is pre-existing (PHYS_PDPT), but PDPTE is absent → alloc PD */
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    alloc_pool[0] = PHYS_ALLOC0;  /* new PD */
    alloc_pool[1] = PHYS_ALLOC1;  /* new PT */
    alloc_count = 2;

    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK");
    /* PDPTE for the new PD table: must be phys|0x3 */
    uint64_t pdpte = mock_pdpt[pdpt_idx];
    CHECK((pdpte & 0x3ULL) == 0x3ULL, "PDPTE Present|RW");
    CHECK((pdpte & ~0x3ULL) == PHYS_ALLOC0, "PDPTE phys = alloc0, no extra bits");
    /* PDE is in the newly allocated PD table (alloc0), not old mock_pd */
    uint64_t pde = mock_alloc0[pd_idx];
    CHECK((pde & 0x3ULL) == 0x3ULL, "PDE Present|RW");
    CHECK((pde & ~0x3ULL) == PHYS_ALLOC1, "PDE phys = alloc1, no extra bits");
    PASS();
}

static void test_final_pte_flags(void) {
    TEST("Final PTE flags (0x1B)");
    reset_state();
    wire_all_tables();
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_count = 3;

    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK");
    uint64_t pte = mock_pt[pt_idx];
    CHECK((pte & 1) != 0, "Present");
    CHECK((pte & 2) != 0, "RW");
    CHECK((pte & 8) != 0, "PWT");
    CHECK((pte & 16) != 0, "PCD");
    CHECK((pte & (1ULL << 7)) == 0, "PAT=0 (bit 7)");
    CHECK((pte & 0x000FFFFFFFFFF000ULL) == TEST_PHYS, "PTE phys correct");
    PASS();
}

static void test_zeroing_before_publish(void) {
    TEST("Temporary page zeroing");
    reset_state();
    /* PML4E not present — need PDPT, PD, PT */
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_count = 3;

    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK");
    CHECK(zeroed_call_count == 3, "3 pages zeroed");
    CHECK(zeroed_failure == 0, "all zeroed pages fully zero");
    PASS();
}

/* ---- true PD allocation failure: PML4E present, PDPTE absent, zero pages ---- */
static void test_pmm_fail_pdpt(void) {
    TEST("PMM_ALLOC_FAILED -- PDPT");
    reset_state();
    save_entries();
    /* PML4E not present — needs PDPT, zero pages available */
    volatile void *out = (void *)0xDEAD;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_PMM_ALLOC_FAILED, "expected status 3");
    CHECK(out == (void *)0xDEAD, "output unchanged");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    CHECK(zeroed_call_count == 0, "no pages zeroed");
    PASS();
}

/* ---- true PD allocation failure: PDPT exists, PD alloc fails ---- */
static void test_pmm_fail_pd(void) {
    TEST("PMM_ALLOC_FAILED -- PD");
    reset_state();
    /* PML4E → existing PDPT, PDPTE absent → need PD */
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    save_entries();
    /* alloc_count stays 0 — PD allocation fails immediately */
    volatile void *out = (void *)0xDEAD;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_PMM_ALLOC_FAILED, "expected status 3");
    CHECK(out == (void *)0xDEAD, "output unchanged");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    CHECK(zeroed_call_count == 0, "no pages zeroed");
    PASS();
}

/* ---- partial atomicity: PDPT+PD alloc succeed, PT fails ---- */
static void test_pmm_fail_pt_partial(void) {
    TEST("PMM_ALLOC_FAILED -- PT (partial atomicity)");
    reset_state();
    /* PML4E not present — need PDPT, PD, PT */
    save_entries();  /* all zero */
    alloc_pool[0] = PHYS_ALLOC0;  /* PDPT */
    alloc_pool[1] = PHYS_ALLOC1;  /* PD */
    alloc_count = 2;               /* PT will fail */
    volatile void *out = (void *)0xDEAD;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_PMM_ALLOC_FAILED, "expected status 3");
    CHECK(out == (void *)0xDEAD, "output unchanged");
    /* Atomicity: PML4E must remain 0 — nothing published */
    CHECK(mock_pml4[pml4_idx] == 0, "PML4E unchanged (partial atomicity)");
    CHECK(mock_pdpt[pdpt_idx] == 0, "PDPTE not published");
    CHECK(mock_pd[pd_idx]     == 0, "PDE not published");
    CHECK(mock_pt[pt_idx]     == 0, "PTE not published");
    CHECK(invlpg_count == 0, "invlpg not called");
    CHECK(zeroed_call_count == 2, "PDPT and PD zeroed before PT failure");
    PASS();
}

static void test_large_page_pdpte(void) {
    TEST("LARGE_PAGE_CONFLICT -- PDPTE.PS=1");
    reset_state();
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    mock_pdpt[pdpt_idx] = TEST_PHYS | 0x83ULL;  /* Present|RW|PS=1 (1 GiB) */
    save_entries();
    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_LARGE_PAGE_CONFLICT, "expected status 4");
    CHECK(out == NULL, "output unchanged");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_large_page_pde(void) {
    TEST("LARGE_PAGE_CONFLICT -- PDE.PS=1");
    reset_state();
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    mock_pdpt[pdpt_idx] = PHYS_PD | 0x3ULL;
    mock_pd[pd_idx]     = TEST_PHYS | 0x83ULL;  /* Present|RW|PS=1 (2 MiB) */
    save_entries();
    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_LARGE_PAGE_CONFLICT, "expected status 4");
    CHECK(out == NULL, "output unchanged");
    CHECK_ENTRIES_UNCHANGED();
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_non_present_pdpte_bit7(void) {
    TEST("Non-present PDPTE bit7=1");
    reset_state();
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    /* PDPTE NOT present, but PS bit (bit 7) is 1 — not a conflict */
    mock_pdpt[pdpt_idx] = (1ULL << 7);
    alloc_pool[0] = PHYS_ALLOC0;  /* PD */
    alloc_pool[1] = PHYS_ALLOC1;  /* PT */
    alloc_count = 2;
    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK — non-present PS ignored");
    PASS();
}

static void test_non_present_pde_bit7(void) {
    TEST("Non-present PDE bit7=1");
    reset_state();
    mock_pml4[pml4_idx] = PHYS_PDPT | 0x3ULL;
    mock_pdpt[pdpt_idx] = PHYS_PD | 0x3ULL;
    /* PDE NOT present, but PS bit (bit 7) is 1 — not a conflict */
    mock_pd[pd_idx] = (1ULL << 7);
    alloc_pool[0] = PHYS_ALLOC0;  /* PT */
    alloc_count = 1;
    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK — non-present PS ignored");
    PASS();
}

static void test_pte_conflict_different_phys(void) {
    TEST("PTE_CONFLICT -- different phys");
    reset_state();
    wire_all_tables();
    /* PTE present with different physical, correct flags */
    mock_pt[pt_idx] = 0xDEAD0000ULL | 0x1BULL;
    save_entries();
    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_PTE_CONFLICT, "expected status 5");
    CHECK(out == NULL, "output unchanged");
    CHECK(mock_pt[pt_idx] == saved_pte, "PTE bit-for-bit unchanged");
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_pte_conflict_missing_flags(void) {
    TEST("PTE_CONFLICT -- missing flags");
    reset_state();
    wire_all_tables();
    /* Same physical but only Present|RW — missing PWT|PCD */
    mock_pt[pt_idx] = TEST_PHYS | 0x3ULL;
    save_entries();
    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_PTE_CONFLICT, "expected status 5");
    CHECK(out == NULL, "output unchanged");
    CHECK(mock_pt[pt_idx] == saved_pte, "PTE bit-for-bit unchanged");
    CHECK(invlpg_count == 0, "invlpg not called");
    PASS();
}

static void test_idempotence(void) {
    TEST("Idempotence -- same phys + flags");
    reset_state();
    wire_all_tables();
    /* PTE already correct */
    mock_pt[pt_idx] = TEST_PHYS | 0x1BULL;
    invlpg_count = 0;

    volatile void *out = NULL;
    enum vmm_status s = vmm_map_mmio_page(TEST_PHYS, TEST_HHDM, &out);
    CHECK(s == VMM_OK, "expected VMM_OK");
    uintptr_t expected = (uintptr_t)(TEST_PHYS + TEST_HHDM);
    CHECK((uintptr_t)out == expected, "*out_virtual correct in idempotence");
    CHECK(invlpg_count == 0, "invlpg NOT called in idempotence");
    PASS();
}

static void test_vmm_init(void) {
    TEST("VMM init captures current CR3");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    const struct address_space *kernel = vmm_kernel_address_space();
    const struct address_space *current = vmm_current_address_space();
    CHECK(kernel != NULL && kernel->pml4_phys == PHYS_PML4,
          "kernel address space captures current CR3");
    CHECK(current != NULL && current->pml4_phys == PHYS_PML4,
          "current address space is initially kernel");
    PASS();
}

static void test_create_address_space(void) {
    TEST("Create address space copies kernel high-half");
    reset_state();
    for (int index = 256; index < 512; ++index) {
        mock_pml4[index] = 0x9000000000000000ULL + (uint64_t)index;
    }
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_count = 1;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    CHECK(as.pml4_phys == PHYS_ALLOC0,
          "new address space owns its PML4 page");

    bool low_zero = true;
    bool high_copied = true;
    for (int index = 0; index < 256; ++index) {
        if (mock_alloc0[index] != 0U) {
            low_zero = false;
        }
    }
    for (int index = 256; index < 512; ++index) {
        if (mock_alloc0[index] != mock_pml4[index]) {
            high_copied = false;
        }
    }
    CHECK(low_zero, "new PML4 low half is zeroed");
    CHECK(high_copied, "new PML4 copies kernel high-half entries");
    PASS();
}

static void test_map_in_new_address_space(void) {
    TEST("Map page in a non-current address space");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_pool[3] = PHYS_ALLOC3;
    alloc_count = 4;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    enum vmm_status s = vmm_map_page_in(
        &as, TEST_VIRT, TEST_PHYS2,
        VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE, TEST_HHDM);
    CHECK(s == VMM_OK, "map page in new address space succeeds");
    CHECK(mock_alloc0[virt_pml4_idx] == (PHYS_ALLOC1 | 0x3ULL),
          "new PML4 publishes PDPT with Present|RW");
    CHECK(mock_alloc1[virt_pdpt_idx] == (PHYS_ALLOC2 | 0x3ULL),
          "new PDPT publishes PD with Present|RW");
    CHECK(mock_alloc2[virt_pd_idx] == (PHYS_ALLOC3 | 0x3ULL),
          "new PD publishes PT with Present|RW");
    CHECK(mock_alloc3[virt_pt_idx] == (TEST_PHYS2 | 0x3ULL),
          "new PT publishes requested PTE");
    CHECK(mock_pml4[0] == 0U,
          "mapping in another address space leaves current CR3 root untouched");
    CHECK(invlpg_count == 0,
          "mapping in a non-current address space does not invlpg");
    PASS();
}

static void test_map_conflicts_and_idempotence(void) {
    TEST("Map idempotence and conflicts in new address space");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_pool[3] = PHYS_ALLOC3;
    alloc_count = 4;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    const uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2, flags, TEST_HHDM) ==
              VMM_OK,
          "initial map succeeds");
    mock_alloc3[virt_pt_idx] |= (1ULL << 5U) | (1ULL << 6U);
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2, flags, TEST_HHDM) ==
              VMM_OK,
          "hardware A/D bits do not break idempotence");
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2, flags, TEST_HHDM) ==
              VMM_OK,
          "identical mapping is idempotent");
    const enum vmm_status conflict_status = vmm_map_page_in(
        &as, TEST_VIRT, TEST_PHYS2 + 0x1000ULL, flags, TEST_HHDM);
    CHECK(conflict_status == VMM_PTE_CONFLICT,
          "different physical mapping returns VMM_PTE_CONFLICT");
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2, 0x1BULL,
                          TEST_HHDM) == VMM_PTE_CONFLICT,
          "different flags mapping returns VMM_PTE_CONFLICT");
    PASS();
}

static void test_large_page_conflict_new_address_space(void) {
    TEST("Large page conflict in new address space");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_count = 1;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    mock_alloc0[virt_pml4_idx] = PHYS_ALLOC1 | 0x3ULL;
    mock_alloc1[virt_pdpt_idx] = PHYS_ALLOC2 | 0x3ULL;
    mock_alloc2[virt_pd_idx] = TEST_PHYS2 | 0x83ULL;
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2,
                          VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE,
                          TEST_HHDM) == VMM_LARGE_PAGE_CONFLICT,
          "large PDE in target address space returns conflict");
    PASS();
}

static void test_unmap_new_address_space(void) {
    TEST("Unmap clears PTE and reports absent second time");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_pool[3] = PHYS_ALLOC3;
    alloc_count = 4;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    const uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2, flags, TEST_HHDM) ==
              VMM_OK,
          "initial map succeeds");
    CHECK(vmm_unmap_page_in(&as, TEST_VIRT, TEST_HHDM) == VMM_OK,
          "unmap succeeds");
    CHECK(mock_alloc3[virt_pt_idx] == 0U,
          "unmap clears the PTE");
    CHECK(invlpg_count == 0,
          "unmap in non-current address space does not invlpg");
    CHECK(vmm_unmap_page_in(&as, TEST_VIRT, TEST_HHDM) == VMM_NOT_MAPPED,
          "second unmap returns VMM_NOT_MAPPED");
    PASS();
}

static void test_activate_address_space(void) {
    TEST("Activate writes CR3 and updates current address space");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_count = 1;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    CHECK(mock_cr3 == PHYS_PML4,
          "create does not modify current CR3");
    CHECK(vmm_activate_address_space(&as) == VMM_OK,
          "activate address space succeeds");
    CHECK(mock_cr3 == PHYS_ALLOC0,
          "activate writes mocked CR3");
    CHECK(vmm_current_address_space()->pml4_phys == PHYS_ALLOC0,
          "current address space follows activation");

    struct address_space kernel_copy;
    kernel_copy.pml4_phys = vmm_kernel_address_space()->pml4_phys;
    CHECK(vmm_activate_address_space(&kernel_copy) == VMM_OK,
          "restore kernel address space succeeds");
    CHECK(mock_cr3 == PHYS_PML4,
          "restore writes kernel CR3");
    CHECK(vmm_current_address_space()->pml4_phys == PHYS_PML4,
          "current address space returns to kernel");
    PASS();
}

static void test_destroy_address_space_empty(void) {
    TEST("Destroy empty address space");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_count = 1;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    CHECK(vmm_destroy_address_space(&as, TEST_HHDM) == VMM_OK,
          "destroy empty address space succeeds");
    CHECK(as.pml4_phys == 0U,
          "destroy clears pml4_phys");
    CHECK(free_call_count == 1 && freed_pages[0] == PHYS_ALLOC0,
          "destroy frees the PML4 page");
    CHECK(vmm_current_address_space()->pml4_phys == PHYS_PML4,
          "destroy does not change current address space");
    PASS();
}

static void test_destroy_address_space_mapped(void) {
    TEST("Destroy mapped address space frees owned tables");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_pool[3] = PHYS_ALLOC3;
    alloc_count = 4;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    const uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE;
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2, flags, TEST_HHDM) ==
              VMM_OK,
          "map page before destroy succeeds");
    CHECK(vmm_destroy_address_space(&as, TEST_HHDM) == VMM_OK,
          "destroy mapped address space succeeds");
    CHECK(as.pml4_phys == 0U,
          "destroy clears mapped pml4_phys");
    CHECK(free_call_count == 4,
          "destroy frees PML4 and three lower table pages");
    CHECK(freed_pages[0] == PHYS_ALLOC3 &&
              freed_pages[1] == PHYS_ALLOC2 &&
              freed_pages[2] == PHYS_ALLOC1 &&
              freed_pages[3] == PHYS_ALLOC0,
          "destroy frees lower tables leaf-to-root then PML4");
    CHECK(mock_cr3 == PHYS_PML4,
          "destroy does not alter current CR3");
    PASS();
}

static void test_destroy_address_space_rejects_owned(void) {
    TEST("Destroy rejects kernel/current/null address spaces");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    struct address_space kernel_copy;
    kernel_copy.pml4_phys = vmm_kernel_address_space()->pml4_phys;
    CHECK(vmm_destroy_address_space(&kernel_copy, TEST_HHDM) ==
              VMM_BAD_ADDRESS_SPACE,
          "kernel address space cannot be destroyed");

    alloc_pool[0] = PHYS_ALLOC0;
    alloc_count = 1;
    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create current address space fixture");
    CHECK(vmm_activate_address_space(&as) == VMM_OK,
          "activate address space fixture");
    CHECK(vmm_destroy_address_space(&as, TEST_HHDM) ==
              VMM_BAD_ADDRESS_SPACE,
          "current address space cannot be destroyed");
    CHECK(as.pml4_phys == PHYS_ALLOC0 && mock_cr3 == PHYS_ALLOC0,
          "rejected current destroy leaves state unchanged");
    CHECK(vmm_destroy_address_space(NULL, TEST_HHDM) ==
              VMM_BAD_ADDRESS_SPACE,
          "null address space returns BAD_ADDRESS_SPACE");
    PASS();
}

static void test_map_pmm_rollback(void) {
    TEST("PMM allocation failure rolls back new tables");
    reset_state();
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "vmm_init returns VMM_OK");
    alloc_pool[0] = PHYS_ALLOC0;
    alloc_pool[1] = PHYS_ALLOC1;
    alloc_pool[2] = PHYS_ALLOC2;
    alloc_count = 3;

    struct address_space as;
    CHECK(vmm_create_address_space(&as, TEST_HHDM) == VMM_OK,
          "create address space succeeds");
    CHECK(vmm_map_page_in(&as, TEST_VIRT, TEST_PHYS2,
                          VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE,
                          TEST_HHDM) == VMM_PMM_ALLOC_FAILED,
          "PT allocation failure returns VMM_PMM_ALLOC_FAILED");
    CHECK(free_call_count == 2,
          "two newly allocated table pages are rolled back");
    CHECK((freed_pages[0] == PHYS_ALLOC1 &&
           freed_pages[1] == PHYS_ALLOC2),
          "rolled-back pages are the PDPT and PD");
    CHECK(mock_alloc0[virt_pml4_idx] == 0U,
          "no partial PML4 entry is published");
    CHECK(mock_alloc1[virt_pdpt_idx] == 0U,
          "no partial PDPT entry is published");
    CHECK(mock_alloc2[virt_pd_idx] == 0U,
          "no partial PD entry is published");
    PASS();
}

static void test_status_string_all(void) {
    TEST("status_string -- all values 0..6 non-empty");
    for (int v = VMM_OK; v <= VMM_NOT_MAPPED; ++v) {
        const char *text = vmm_status_string((enum vmm_status)v);
        CHECK(text != NULL && text[0] != '\0', "status string non-empty");
    }
    PASS();
}

/* ================================================================ */

int main(void) {
    init_indices();
    printf("VMM host test\n");

    test_bad_output_pointer();
    test_bad_alignment();
    test_hhdm_overflow();
    test_ok_new_mapping();
    test_intermediate_flags();
    test_final_pte_flags();
    test_zeroing_before_publish();
    test_pmm_fail_pdpt();
    test_pmm_fail_pd();
    test_pmm_fail_pt_partial();
    test_large_page_pdpte();
    test_large_page_pde();
    test_non_present_pdpte_bit7();
    test_non_present_pde_bit7();
    test_pte_conflict_different_phys();
    test_pte_conflict_missing_flags();
    test_idempotence();
    test_vmm_init();
    test_create_address_space();
    test_map_in_new_address_space();
    test_map_conflicts_and_idempotence();
    test_large_page_conflict_new_address_space();
    test_unmap_new_address_space();
    test_activate_address_space();
    test_destroy_address_space_empty();
    test_destroy_address_space_mapped();
    test_destroy_address_space_rejects_owned();
    test_map_pmm_rollback();
    test_status_string_all();

    if (failures) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("\nVMM host test: OK\n");
    return 0;
}
