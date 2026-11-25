/*
 * ITEM-390 high-memory PMM host proof.
 *
 * The kernel used to refuse to initialise above HFOC_PMM_MAX_PAGES (1U << 22
 * pages = 16 GiB), which panicked the boot on a machine with more memory than
 * every 512 MiB QEMU proof ever exercised. This proof drives pmm_init() with a
 * synthetic, fragmented memory map that is deliberately larger than that legacy
 * cap and asserts that the initialisation succeeds with full coverage, that the
 * bitmap lands in a region that can actually hold it, and that allocations stay
 * inside tracked memory.
 *
 * The map is mapped with MAP_NORESERVE: the test only ever touches the bitmap
 * pages and the pages it allocates, so a 32 GiB address span costs a few MiB of
 * committed host memory.
 */
#ifdef HFOC_PMM_HIGHMEM_HOST_TEST

#include "pmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define PAGE_SIZE 4096ULL
#define MIB (1024ULL * 1024ULL)
#define GIB (1024ULL * MIB)

/* The legacy page cap this proof exists to exceed. */
#define LEGACY_MAX_PAGES (1U << 22U)

/* Address span of the synthetic machine: RAM plus the top firmware hole. */
#define ADDRESS_SPAN (32ULL * GIB)

/* Fragment layout, in bytes. The first usable range is smaller than the page
 * bitmap itself; that is exactly the shape that made the old regions[0]
 * placement fail on a fragmented firmware map. The largest range sits above
 * the 4 GiB MMIO hole. */
#define REGION_A_BASE 0x100000ULL
#define REGION_A_SIZE (256ULL * 1024ULL)
#define HOLE_ONE_BASE (REGION_A_BASE + REGION_A_SIZE)
#define HOLE_ONE_SIZE (8ULL * MIB)
#define REGION_B_BASE (HOLE_ONE_BASE + HOLE_ONE_SIZE)
#define REGION_B_SIZE (4ULL * GIB)
#define REGION_C_BASE (0x100000000ULL + 4ULL * GIB)
#define REGION_C_SIZE (8ULL * GIB)
#define REGION_D_BASE (REGION_C_BASE + REGION_C_SIZE + 16ULL * MIB)
#define REGION_D_SIZE (6ULL * GIB)
#define REGION_E_BASE (REGION_D_BASE + REGION_D_SIZE + 32ULL * MIB)
#define REGION_E_SIZE (3ULL * GIB)

struct known_region {
    uint64_t base;
    uint64_t size;
};

static const struct known_region known_regions[] = {
    { REGION_A_BASE, REGION_A_SIZE },
    { REGION_B_BASE, REGION_B_SIZE },
    { REGION_C_BASE, REGION_C_SIZE },
    { REGION_D_BASE, REGION_D_SIZE },
    { REGION_E_BASE, REGION_E_SIZE }
};

static struct limine_memmap_entry entries[16];
static struct limine_memmap_entry *entry_ptrs[16];
static struct limine_memmap_response response;
static int failures;
static unsigned char *arena;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

static void add_entry(uint64_t base, uint64_t length, uint64_t type,
                      uint64_t *count) {
    entries[*count].base = base;
    entries[*count].length = length;
    entries[*count].type = type;
    entry_ptrs[*count] = &entries[*count];
    ++(*count);
}

static void setup_high_memory_map(void) {
    uint64_t count = 0U;
    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));

    /* Firmware reserved low memory plus a usable piece below 1 MiB that the
     * allocator intentionally ignores. */
    add_entry(0U, 0x100000ULL, LIMINE_MEMMAP_RESERVED, &count);
    add_entry(0x90000ULL, 0x10000ULL, LIMINE_MEMMAP_USABLE, &count);

    /* The small first usable range. */
    add_entry(REGION_A_BASE, REGION_A_SIZE, LIMINE_MEMMAP_USABLE, &count);
    /* A firmware hole. */
    add_entry(HOLE_ONE_BASE, HOLE_ONE_SIZE, LIMINE_MEMMAP_ACPI_NVS, &count);
    /* Overlapping and adjacent usable pieces: a firmware map may report the
     * same memory twice or in touching pieces, and the page count must not
     * inflate because of it. */
    add_entry(REGION_B_BASE, 2ULL * GIB, LIMINE_MEMMAP_USABLE, &count);
    add_entry(REGION_B_BASE + 1ULL * GIB, 2ULL * GIB,
              LIMINE_MEMMAP_USABLE, &count);
    add_entry(REGION_B_BASE + 2ULL * GIB, 2ULL * GIB,
              LIMINE_MEMMAP_USABLE, &count);
    /* RAM above the 4 GiB MMIO hole, fragmented like real firmware. */
    add_entry(REGION_C_BASE, REGION_C_SIZE, LIMINE_MEMMAP_USABLE, &count);
    add_entry(REGION_C_BASE + REGION_C_SIZE, 16ULL * MIB,
              LIMINE_MEMMAP_RESERVED, &count);
    add_entry(REGION_D_BASE, REGION_D_SIZE, LIMINE_MEMMAP_USABLE, &count);
    add_entry(REGION_D_BASE + REGION_D_SIZE, 32ULL * MIB,
              LIMINE_MEMMAP_RESERVED, &count);
    add_entry(REGION_E_BASE, REGION_E_SIZE, LIMINE_MEMMAP_USABLE, &count);

    response.revision = 0U;
    response.entry_count = count;
    response.entries = entry_ptrs;
}

static bool initialise_high_memory(void) {
    setup_high_memory_map();
    return pmm_init(&response, (uint64_t)(uintptr_t)arena);
}

static uint64_t usable_entry_bytes(void) {
    uint64_t total = 0U;
    for (uint64_t index = 0U; index < response.entry_count; ++index) {
        if (entries[index].type == LIMINE_MEMMAP_USABLE) {
            total += entries[index].length;
        }
    }
    return total;
}

static bool page_is_zero(uint64_t physical) {
    const unsigned char *page = arena + physical;
    for (size_t index = 0U; index < PAGE_SIZE; ++index) {
        if (page[index] != 0U) {
            return false;
        }
    }
    return true;
}

static uint64_t region_end_containing(uint64_t physical) {
    for (size_t index = 0U;
         index < sizeof(known_regions) / sizeof(known_regions[0]); ++index) {
        if (physical >= known_regions[index].base &&
            physical < known_regions[index].base + known_regions[index].size) {
            return known_regions[index].base + known_regions[index].size;
        }
    }
    return 0U;
}

static bool inside_known_region(uint64_t physical) {
    return region_end_containing(physical) != 0U;
}

static void test_map_is_larger_than_legacy_cap(void) {
    printf("synthetic map exceeds the legacy 16 GiB cap\n");
    setup_high_memory_map();
    CHECK(usable_entry_bytes() > (uint64_t)LEGACY_MAX_PAGES * PAGE_SIZE,
          "synthetic usable memory is above the legacy 16 GiB cap");
}

static void test_high_memory_initialises(void) {
    printf("high memory initialises with full coverage\n");
    CHECK(initialise_high_memory(),
          "fragmented map above the legacy cap initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->status == PMM_INIT_STATUS_OK, "status is OK");
    CHECK(report->coverage == PMM_COVERAGE_FULL,
          "coverage is FULL: nothing usable was dropped");
    CHECK(report->dropped_pages == 0U, "no usable page was dropped");
    CHECK(report->dropped_regions == 0U, "no usable region was dropped");
    CHECK(report->total_pages > (uint64_t)LEGACY_MAX_PAGES,
          "tracked pages exceed the legacy cap");
    CHECK(report->page_cap > report->total_pages,
          "the shipped page cap is above the machine");
    CHECK(report->tracked_bytes == report->total_pages * PAGE_SIZE,
          "tracked bytes match the tracked page count");
    CHECK(report->usable_bytes == usable_entry_bytes(),
          "usable bytes match the memory map");
    CHECK(report->region_count == 5U,
          "overlapping and adjacent usable ranges coalesce into five regions");
    CHECK(pmm_total_usable_bytes() == report->usable_bytes,
          "the allocator publishes the same usable byte count");
}

static void test_bitmap_placement(void) {
    printf("bitmap placement\n");
    CHECK(initialise_high_memory(),
          "fragmented map above the legacy cap initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->bitmap_pages > 0U, "the bitmap reserves pages");
    CHECK(report->bitmap_phys != 0U, "the bitmap has a physical address");
    CHECK(REGION_A_SIZE / PAGE_SIZE < (uint64_t)report->bitmap_pages,
          "the first usable region cannot hold the bitmap at all");
    CHECK(inside_known_region(report->bitmap_phys),
          "the bitmap lives inside a usable region");
    CHECK(report->largest_region_pages >= (uint64_t)report->bitmap_pages,
          "the largest region can hold the bitmap");
    CHECK(report->bitmap_phys != REGION_A_BASE,
          "the bitmap is not forced into the first region, which is too small");
    CHECK(report->bitmap_phys == REGION_C_BASE,
          "the bitmap is taken from the largest usable region");
    CHECK(report->placement_region == 2U,
          "the placement region is the largest one after coalescing");
    const uint64_t bitmap_end =
        report->bitmap_phys + (uint64_t)report->bitmap_pages * PAGE_SIZE;
    CHECK(bitmap_end <= region_end_containing(report->bitmap_phys),
          "the bitmap stays inside the region it was taken from");
    CHECK(!pmm_free_page(report->bitmap_phys),
          "the first bitmap page can never be freed or handed out");
    CHECK(!pmm_free_page(bitmap_end - PAGE_SIZE),
          "the last bitmap page is reserved as well");
}

static void test_free_page_accounting(void) {
    printf("free page accounting\n");
    CHECK(initialise_high_memory(),
          "fragmented map above the legacy cap initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(pmm_free_page_count() == report->total_pages - report->bitmap_pages,
          "free pages are all tracked pages except the bitmap");
    CHECK(pmm_free_page_count() > (uint64_t)LEGACY_MAX_PAGES,
          "the allocator can hand out more pages than the legacy cap");
}

static void test_allocation_stays_tracked(void) {
    printf("allocation stays inside tracked memory\n");
    CHECK(initialise_high_memory(),
          "fragmented map above the legacy cap initialises");
    const uint64_t free_before = pmm_free_page_count();
    const uint64_t first = pmm_alloc_page();
    const uint64_t second = pmm_alloc_page();
    CHECK(first != 0U && second != 0U, "allocation succeeds");
    CHECK(first != second, "allocated pages are unique");
    CHECK(inside_known_region(first) && inside_known_region(second),
          "allocated pages are inside usable regions");
    CHECK(first % PAGE_SIZE == 0U && second % PAGE_SIZE == 0U,
          "allocated pages are 4 KiB aligned");
    CHECK(page_is_zero(first), "allocated page is zeroed through the HHDM");
    CHECK(pmm_free_page_count() == free_before - 2U,
          "free count decreases by two");
    CHECK(pmm_free_page(first), "an allocated page can be freed");
    CHECK(pmm_free_page_count() == free_before - 1U,
          "free count increases after a free");
    const uint64_t contiguous = pmm_alloc_contiguous_pages(3U);
    CHECK(contiguous != 0U, "contiguous allocation succeeds");
    CHECK(inside_known_region(contiguous) &&
          inside_known_region(contiguous + 2U * PAGE_SIZE),
          "the contiguous run stays inside tracked memory");
}

static void test_diagnostic_render(void) {
    printf("diagnostic rendering\n");
    char line[HFOC_PMM_DIAGNOSTIC_MAX];
    CHECK(initialise_high_memory(),
          "fragmented map above the legacy cap initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    const size_t written =
        pmm_init_diagnostic_render(report, line, sizeof(line));
    CHECK(written > 0U && written + 1U <= sizeof(line),
          "the one-line diagnostic fits its documented bound");
    CHECK(strstr(line, "status=OK") == line, "the line starts with the status");
    CHECK(strstr(line, "coverage=FULL") != NULL, "the line names the coverage");
    CHECK(strstr(line, "dropped_pages=0") != NULL, "the line names the drops");
    CHECK(strstr(line, "bitmap_phys=") != NULL,
          "the line names the bitmap placement");

    /* Worst case: every numeric field at its maximum must still fit. */
    const struct pmm_init_diagnostic worst = {
        .status = PMM_INIT_ERR_ARITHMETIC_OVERFLOW,
        .coverage = PMM_COVERAGE_LIMITED,
        .usable_bytes = UINT64_MAX,
        .tracked_bytes = UINT64_MAX,
        .dropped_pages = UINT64_MAX,
        .total_pages = UINT64_MAX,
        .page_cap = UINT64_MAX,
        .region_count = UINT32_MAX,
        .dropped_regions = UINT32_MAX,
        .bitmap_pages = UINT32_MAX,
        .largest_region_pages = UINT64_MAX,
        .placement_region = UINT32_MAX,
        .bitmap_phys = UINT64_MAX
    };
    const size_t worst_written =
        pmm_init_diagnostic_render(&worst, line, sizeof(line));
    CHECK(worst_written + 1U <= sizeof(line),
          "the widest possible diagnostic still fits the documented bound");
    CHECK((size_t)strlen(line) == worst_written,
          "the widest possible diagnostic is not truncated");
    CHECK(strstr(line, "status=ERR_ARITHMETIC_OVERFLOW") == line,
          "the failure reason is rendered verbatim");

    struct pmm_init_diagnostic copy = worst;
    copy.status = PMM_INIT_ERR_NO_BITMAP_PLACEMENT;
    CHECK(strstr(line, "status=ERR_NO_BITMAP_PLACEMENT") == NULL,
          "the renderer does not mutate the diagnostic it renders");
    (void)pmm_init_diagnostic_render(&copy, line, sizeof(line));
    CHECK(strstr(line, "status=ERR_NO_BITMAP_PLACEMENT") == line,
          "a second reason renders verbatim");

    char small[8];
    const size_t small_written =
        pmm_init_diagnostic_render(report, small, sizeof(small));
    CHECK(small_written == written,
          "truncated rendering still reports the full length");
    CHECK(small[sizeof(small) - 1U] == '\0',
          "truncated rendering still terminates the buffer");
}

int main(void) {
    printf("PMM high-memory host test\n");

    arena = mmap(NULL, ADDRESS_SPAN, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (arena == MAP_FAILED) {
        printf("FAIL: cannot reserve %llu bytes of address space\n",
               (unsigned long long)ADDRESS_SPAN);
        printf("\n1 FAILURES\n");
        return 1;
    }

    test_map_is_larger_than_legacy_cap();
    test_high_memory_initialises();
    test_bitmap_placement();
    test_free_page_accounting();
    test_allocation_stays_tracked();
    test_diagnostic_render();

    if (munmap(arena, ADDRESS_SPAN) != 0) {
        printf("FAIL: address space is released\n");
        ++failures;
    }

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("PMM high-memory host test: ALL OK");
    return 0;
}

#else
#error "tests/pmm-highmem-host.c requires -DHFOC_PMM_HIGHMEM_HOST_TEST"
#endif
