#include "pmm.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define MEM_BASE   0x100000ULL
#define MEM_SIZE   (3U * 1024U * 1024U)
#define PAGE_SIZE  4096ULL

static unsigned char memory[MEM_SIZE] __attribute__((aligned(4096)));

static struct limine_memmap_entry entries[8];
static struct limine_memmap_entry *entry_ptrs[8];
static struct limine_memmap_response response;
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        printf("FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void *phys_to_ptr(uint64_t physical) {
    return memory + (physical - MEM_BASE);
}

static bool page_is_zero(uint64_t physical) {
    const unsigned char *page = phys_to_ptr(physical);
    for (size_t index = 0U; index < PAGE_SIZE; ++index) {
        if (page[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void fill_memory(void) {
    memset(memory, 0xA5, sizeof(memory));
}

static void setup_standard_map(void) {
    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));
    entries[0] = (struct limine_memmap_entry){
        .base = 0U, .length = 0x100000ULL, .type = LIMINE_MEMMAP_RESERVED
    };
    entries[1] = (struct limine_memmap_entry){
        .base = 0x90000ULL, .length = 0x10000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entries[2] = (struct limine_memmap_entry){
        .base = 0x100000ULL, .length = 0x200000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entries[3] = (struct limine_memmap_entry){
        .base = 0x300000ULL, .length = 0x100000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entries[4] = (struct limine_memmap_entry){
        .base = 0x500000ULL, .length = 0x100000ULL,
        .type = LIMINE_MEMMAP_RESERVED
    };
    for (size_t index = 0U; index < 5U; ++index) {
        entry_ptrs[index] = &entries[index];
    }
    response.revision = 0U;
    response.entry_count = 5U;
    response.entries = entry_ptrs;
}

static bool init_standard_pmm(void) {
    fill_memory();
    setup_standard_map();
    const uint64_t hhdm_offset =
        (uint64_t)(uintptr_t)memory - MEM_BASE;
    return pmm_init(&response, hhdm_offset);
}

static void test_usable_byte_count(void) {
    printf("usable byte count\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    CHECK(pmm_total_usable_bytes() == 0x310000ULL,
          "only usable entries count toward usable bytes");
}

static void test_low_memory_and_bitmap_reservation(void) {
    printf("low memory and bitmap reservation\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    CHECK(pmm_free_page_count() == 767U,
          "one page is reserved for the bitmap");
    const uint64_t first = pmm_alloc_page();
    CHECK(first == 0x101000ULL,
          "first allocation starts above 1 MiB and after bitmap page");
    CHECK(first != 0x100000ULL, "bitmap page is never returned");
    CHECK((first & 0xFFFULL) == 0U, "allocated page is 4 KiB aligned");
}

static void test_alloc_free_reuse(void) {
    printf("alloc/free/reuse\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    const uint64_t first = pmm_alloc_page();
    const uint64_t second = pmm_alloc_page();
    const uint64_t third = pmm_alloc_page();
    CHECK(first != 0U && second != 0U && third != 0U,
          "allocation succeeds");
    CHECK(first != second && first != third && second != third,
          "allocated pages are unique");
    CHECK(third == first + 2U * PAGE_SIZE,
          "bitmap allocator returns consecutive pages from first region");
    CHECK(page_is_zero(first), "allocated page is zeroed through HHDM");
    CHECK(pmm_free_page_count() == 764U,
          "free count decreases after three allocations");
    CHECK(pmm_free_page(first), "freeing an allocated page succeeds");
    CHECK(pmm_free_page_count() == 765U,
          "free count increases after free");
    CHECK(pmm_alloc_page() == first,
          "freed page is allocatable again");
}

static void test_invalid_free(void) {
    printf("invalid free\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    const uint64_t free_before = pmm_free_page_count();
    CHECK(!pmm_free_page(0U), "physical address zero is rejected");
    CHECK(!pmm_free_page(0x100000ULL), "bitmap page is rejected");
    CHECK(!pmm_free_page(0x200000ULL), "untracked hole is rejected");
    CHECK(!pmm_free_page(0x3FF001ULL), "unaligned page is rejected");
    CHECK(!pmm_free_page(0x800000000000ULL),
          "address outside bitmap capacity is rejected");
    CHECK(pmm_free_page_count() == free_before,
          "invalid frees leave bitmap unchanged");
    const uint64_t allocated = pmm_alloc_page();
    CHECK(allocated == 0x101000ULL,
          "allocated page is available for a valid free");
    CHECK(pmm_free_page(allocated), "freeing an allocated page succeeds");
    CHECK(!pmm_free_page(allocated),
          "freeing a non-allocated page is rejected");
    CHECK(pmm_free_page_count() == free_before,
          "double free leaves bitmap unchanged");
}

static void test_contiguous_alloc(void) {
    printf("contiguous allocation\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    const uint64_t first = pmm_alloc_contiguous_pages(4U);
    CHECK(first == 0x101000ULL, "contiguous run starts after bitmap");
    CHECK(pmm_alloc_page() == 0x105000ULL,
          "next single allocation skips the whole contiguous run");
    CHECK(page_is_zero(first) && page_is_zero(first + PAGE_SIZE) &&
          page_is_zero(first + 2U * PAGE_SIZE) &&
          page_is_zero(first + 3U * PAGE_SIZE),
          "contiguous pages are zeroed");
    CHECK(pmm_free_contiguous_pages(first, 4U),
          "contiguous free succeeds");
    CHECK(pmm_free_page(0x105000ULL),
          "following single page can be freed");
    CHECK(pmm_alloc_page() == first,
          "freed contiguous run is allocatable again");
    CHECK(!pmm_alloc_contiguous_pages(0U),
          "zero-length contiguous allocation is rejected");
}

static void test_invalid_contiguous_free(void) {
    printf("invalid contiguous free\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    const uint64_t before = pmm_free_page_count();
    CHECK(!pmm_free_contiguous_pages(0U, 1U),
          "zero physical address is rejected");
    CHECK(!pmm_free_contiguous_pages(0x100000ULL, 1U),
          "bitmap page range is rejected");
    CHECK(!pmm_free_contiguous_pages(0x200000ULL, 1U),
          "untracked range is rejected");
    CHECK(!pmm_free_contiguous_pages(0x101000ULL, 0U),
          "zero-length contiguous free is rejected");
    CHECK(pmm_free_page_count() == before,
          "invalid contiguous frees leave bitmap unchanged");

    CHECK(pmm_alloc_contiguous_pages(2U) == 0x101000ULL,
          "two-page allocation succeeds");
    CHECK(!pmm_free_contiguous_pages(0x101000ULL, 3U),
          "freeing an oversized run with a free tail is rejected");
    CHECK(pmm_free_page_count() == before - 2U,
          "oversized free does not change allocation state");
}

static void test_exhaustion(void) {
    printf("allocation exhaustion\n");
    CHECK(init_standard_pmm(), "standard map initialises");
    uint64_t last = 0U;
    unsigned int count = 0U;
    for (;;) {
        const uint64_t page = pmm_alloc_page();
        if (page == 0U) {
            break;
        }
        last = page;
        ++count;
        if (count > 768U) {
            break;
        }
    }
    CHECK(count == 767U, "all usable pages except bitmap are allocated");
    CHECK(pmm_alloc_page() == 0U, "exhausted allocator returns zero");
    CHECK(pmm_free_page_count() == 0U,
          "free count reaches zero after exhaustion");
    CHECK(pmm_free_page(last), "freed exhausted page is accepted");
    CHECK(pmm_alloc_page() == last,
          "exhausted allocator hands out the freed page");
}

/*
 * ITEM-390 replaced the old "a map above HFOC_PMM_MAX_PAGES fails closed"
 * contract: that failure was the production panic on real hardware, and the
 * kernel now boots with the memory the bitmap can cover and publishes the drop
 * (tests/pmm-limited-host.c drives the limited-coverage path with a reduced
 * cap, tests/pmm-highmem-host.c drives a map above the legacy 16 GiB cap with
 * the production value, and the QEMU PMM proof boots the production ISO on a
 * machine whose RAM is above the legacy cap). What stays pinned here is that
 * the shipped cap is far above ordinary machines and that the diagnostic
 * always publishes the bound that applies.
 */
static void test_shipped_cap_covers_ordinary_machines(void) {
    printf("shipped capacity covers ordinary machines\n");
    CHECK((uint64_t)HFOC_PMM_MAX_PAGES > (uint64_t)(1U << 22U),
          "the shipped cap is above the legacy 16 GiB limit");
    CHECK((uint64_t)HFOC_PMM_MAX_PAGES * PAGE_SIZE >= (1ULL << 40U),
          "the shipped cap covers at least 1 TiB of RAM");
    CHECK(HFOC_PMM_DIAGNOSTIC_MAX >= 344U,
          "the diagnostic buffer holds the widest possible line");
    CHECK(init_standard_pmm(), "the standard map initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->page_cap == (uint64_t)HFOC_PMM_MAX_PAGES,
          "the diagnostic publishes the cap that applies");
    CHECK(report->usable_bytes == 0x310000ULL,
          "the diagnostic publishes the usable bytes it saw");
    CHECK(report->coverage == PMM_COVERAGE_FULL,
          "the standard map is fully covered");
    CHECK(report->dropped_pages == 0U, "nothing is dropped below the cap");
}

static void test_bitmap_placement_prefers_a_region_that_fits(void) {
    printf("bitmap placement prefers a region that fits\n");
    fill_memory();
    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));
    /* A tiny first usable range followed by a large one: the bitmap cannot
     * live in the first region, which is what the old code assumed. */
    entries[0] = (struct limine_memmap_entry){
        .base = 0x100000ULL, .length = 0x10000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entries[1] = (struct limine_memmap_entry){
        .base = 0x200000ULL, .length = 0x200000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entry_ptrs[0] = &entries[0];
    entry_ptrs[1] = &entries[1];
    response.revision = 0U;
    response.entry_count = 2U;
    response.entries = entry_ptrs;
    const uint64_t hhdm_offset = (uint64_t)(uintptr_t)memory - MEM_BASE;
    CHECK(pmm_init(&response, hhdm_offset), "the fragmented map initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->coverage == PMM_COVERAGE_FULL,
          "both usable ranges are tracked");
    CHECK(report->bitmap_phys == 0x200000ULL,
          "the bitmap is taken from the region that can hold it");
    CHECK(report->placement_region == 1U,
          "the placement region is published");
    CHECK(!pmm_free_page(0x200000ULL), "the bitmap page is reserved");
    CHECK(pmm_alloc_page() == 0x100000ULL,
          "allocation starts at the first usable page of the first region");
}

static void test_overlapping_entries_do_not_inflate_the_count(void) {
    printf("overlapping and adjacent entries do not inflate the count\n");
    fill_memory();
    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));
    /* The same 2 MiB reported three times, twice overlapping and once
     * adjacent: a firmware map may look like this and must not be counted
     * three times against the cap. */
    entries[0] = (struct limine_memmap_entry){
        .base = 0x100000ULL, .length = 0x200000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entries[1] = (struct limine_memmap_entry){
        .base = 0x200000ULL, .length = 0x200000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    entries[2] = (struct limine_memmap_entry){
        .base = 0x300000ULL, .length = 0x200000ULL,
        .type = LIMINE_MEMMAP_USABLE
    };
    for (size_t index = 0U; index < 3U; ++index) {
        entry_ptrs[index] = &entries[index];
    }
    response.revision = 0U;
    response.entry_count = 3U;
    response.entries = entry_ptrs;
    const uint64_t hhdm_offset = (uint64_t)(uintptr_t)memory - MEM_BASE;
    CHECK(pmm_init(&response, hhdm_offset), "the fragmented map initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->region_count == 1U, "the three entries coalesce into one");
    CHECK(report->total_pages == 0x400000ULL / PAGE_SIZE,
          "the tracked page count is the coalesced size, not the naive sum");
    CHECK(report->usable_bytes == 0x600000ULL,
          "usable bytes still report what the map declared");
    CHECK(report->coverage == PMM_COVERAGE_FULL,
          "coalescing keeps the coverage full");
}

static void test_failure_paths_publish_a_reason(void) {
    printf("failure paths publish a reason\n");
    const uint64_t hhdm_offset = (uint64_t)(uintptr_t)memory - MEM_BASE;
    char line[HFOC_PMM_DIAGNOSTIC_MAX];

    CHECK(!pmm_init(NULL, hhdm_offset), "a NULL memory map fails");
    CHECK(pmm_init_diagnostic()->status == PMM_INIT_ERR_NO_MEMMAP,
          "a NULL memory map reports ERR_NO_MEMMAP");

    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));
    response.revision = 0U;
    response.entry_count = 1U;
    response.entries = entry_ptrs;
    CHECK(!pmm_init(&response, hhdm_offset), "a NULL entry fails");
    CHECK(pmm_init_diagnostic()->status == PMM_INIT_ERR_NULL_ENTRY,
          "a NULL entry reports ERR_NULL_ENTRY");

    const size_t written = pmm_init_diagnostic_render(
        pmm_init_diagnostic(), line, sizeof(line));
    CHECK(written > 0U && written + 1U <= sizeof(line),
          "the reason renders inside the documented bound");
    CHECK(strstr(line, "ERR_NULL_ENTRY") != NULL,
          "the rendered line names the reason");
    CHECK(strstr(line, "cap=") != NULL, "the rendered line names the cap");
    CHECK(pmm_alloc_page() == 0U, "a failed initialisation allocates nothing");
    CHECK(pmm_free_page_count() == 0U,
          "a failed initialisation reports zero free pages");
}

int main(void) {
    printf("PMM host test\n");
    test_usable_byte_count();
    test_low_memory_and_bitmap_reservation();
    test_alloc_free_reuse();
    test_invalid_free();
    test_contiguous_alloc();
    test_invalid_contiguous_free();
    test_exhaustion();
    test_shipped_cap_covers_ordinary_machines();
    test_bitmap_placement_prefers_a_region_that_fits();
    test_overlapping_entries_do_not_inflate_the_count();
    test_failure_paths_publish_a_reason();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("PMM host test: OK");
    return 0;
}
