/*
 * ITEM-390 PMM cap host proof.
 *
 * The kernel must never panic because a machine has more RAM than the page
 * bitmap can cover. This proof compiles kernel/src/pmm.c with a deliberately
 * small HFOC_PMM_MAX_PAGES (256 MiB) and drives pmm_init() with synthetic maps
 * that exceed it, asserting that:
 *
 *   - the boot continues instead of failing closed;
 *   - the covered prefix is tracked and the rest is published honestly as
 *     coverage=LIMITED with dropped_pages/dropped_regions;
 *   - untracked memory is never handed out by the allocator;
 *   - every failure path leaves its own specific reason for the caller.
 *
 * The synthetic address space is mapped with MAP_NORESERVE and only the
 * tracked memory is ever touched.
 */
#ifdef HFOC_PMM_LIMITED_HOST_TEST

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

/* This proof only makes sense with the reduced cap the Makefile pins. */
#if HFOC_PMM_MAX_PAGES != (1U << 16U)
#error "tests/pmm-limited-host.c requires -DHFOC_PMM_MAX_PAGES=(1U << 16U)"
#endif

#define PAGE_CAP ((uint64_t)HFOC_PMM_MAX_PAGES)

#define ARENA_SPAN (1ULL * GIB)

/* Main map: one huge usable range that is truncated by the cap, plus a later
 * usable range that falls entirely outside the covered prefix. */
#define BIG_BASE 0x100000ULL
#define BIG_SIZE (512ULL * MIB)
#define TAIL_BASE (BIG_BASE + BIG_SIZE + 8ULL * MIB)
#define TAIL_SIZE (4ULL * MIB)

/* Region-table map: one usable range big enough for the bitmap followed by
 * more disjoint usable ranges than the table can hold. */
#define TABLE_BASE 0x100000ULL
#define TABLE_SIZE (64ULL * MIB)
#define TABLE_PIECES 300U
#define TABLE_PIECE_SIZE 0x1000ULL
#define TABLE_PIECE_GAP 0x1000ULL
#define TABLE_FIRST_PIECE (TABLE_BASE + TABLE_SIZE + TABLE_PIECE_GAP)
/* The region table bound pinned by kernel/src/pmm.c (MAX_REGIONS). */
#define ADMITTED_REGIONS 256U
#define TABLE_PIECE_STRIDE (TABLE_PIECE_SIZE + TABLE_PIECE_GAP)

static uint64_t piece_base(uint32_t index) {
    return TABLE_FIRST_PIECE + (uint64_t)index * TABLE_PIECE_STRIDE;
}

static uint64_t last_admitted_piece_base(void) {
    return piece_base(ADMITTED_REGIONS - 2U);
}

static uint64_t last_dropped_piece_base(void) {
    return piece_base(TABLE_PIECES - 1U);
}

static struct limine_memmap_entry entries[1024];
static struct limine_memmap_entry *entry_ptrs[1024];
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

static void map_reset(void) {
    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));
    response.revision = 0U;
    response.entry_count = 0U;
    response.entries = entry_ptrs;
}

static void map_add(uint64_t base, uint64_t length, uint64_t type) {
    const uint64_t count = response.entry_count;
    entries[count].base = base;
    entries[count].length = length;
    entries[count].type = type;
    entry_ptrs[count] = &entries[count];
    response.entry_count = count + 1U;
}

static void map_main(void) {
    map_reset();
    map_add(0U, 0x100000ULL, LIMINE_MEMMAP_RESERVED);
    map_add(BIG_BASE, BIG_SIZE, LIMINE_MEMMAP_USABLE);
    map_add(BIG_BASE + BIG_SIZE, 8ULL * MIB, LIMINE_MEMMAP_ACPI_NVS);
    map_add(TAIL_BASE, TAIL_SIZE, LIMINE_MEMMAP_USABLE);
}

static void map_region_table(void) {
    map_reset();
    map_add(0U, 0x100000ULL, LIMINE_MEMMAP_RESERVED);
    map_add(TABLE_BASE, TABLE_SIZE, LIMINE_MEMMAP_USABLE);
    for (uint32_t index = 0U; index < TABLE_PIECES; ++index) {
        const uint64_t base =
            TABLE_FIRST_PIECE + (uint64_t)index * (TABLE_PIECE_SIZE +
                                                   TABLE_PIECE_GAP);
        map_add(base, TABLE_PIECE_SIZE, LIMINE_MEMMAP_USABLE);
    }
}

static const char *rendered_status(void) {
    static char line[HFOC_PMM_DIAGNOSTIC_MAX];
    (void)pmm_init_diagnostic_render(pmm_init_diagnostic(), line, sizeof(line));
    return line;
}

static void expect_failure(const char *label, enum pmm_init_status status) {
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->status == status, label);
    CHECK(report->coverage == PMM_COVERAGE_NONE,
          "a failed initialisation never claims coverage");
    CHECK(strncmp(rendered_status(), "status=", 7U) == 0,
          "the diagnostic line starts with the status");
    CHECK(strstr(rendered_status(), pmm_init_status_string(status)) != NULL,
          "the diagnostic line names the specific reason");
    CHECK(pmm_alloc_page() == 0U,
          "a failed initialisation hands out no page");
    CHECK(pmm_free_page_count() == 0U,
          "a failed initialisation reports no free page");
    CHECK(!pmm_free_page(BIG_BASE), "a failed initialisation frees nothing");
}

static void test_over_cap_boot_continues(void) {
    printf("memory above the cap does not stop the boot\n");
    map_main();
    CHECK(pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a map above the page cap initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->status == PMM_INIT_STATUS_OK, "status is OK");
    CHECK(report->coverage == PMM_COVERAGE_LIMITED,
          "coverage is published as LIMITED");
    CHECK(report->page_cap == PAGE_CAP, "the reduced cap is in force");
    CHECK(report->total_pages == PAGE_CAP,
          "the tracked prefix stops exactly at the cap");
    CHECK(report->tracked_bytes == PAGE_CAP * PAGE_SIZE,
          "tracked bytes stop exactly at the cap");
    CHECK(report->usable_bytes == BIG_SIZE + TAIL_SIZE,
          "the full usable byte count is still reported honestly");
    CHECK(report->dropped_pages ==
              (BIG_SIZE + TAIL_SIZE) / PAGE_SIZE - PAGE_CAP,
          "dropped pages count everything above the covered prefix");
    CHECK(report->dropped_regions == 0U,
          "the region table holds this map");
    CHECK(report->region_count == 1U,
          "the later usable range is entirely outside the covered prefix");
    CHECK(report->bitmap_pages > 0U, "the bitmap reserves pages");
    CHECK(report->placement_region == 0U,
          "the bitmap is placed in the only tracked region");
    CHECK(report->bitmap_phys == BIG_BASE,
          "the bitmap starts at the beginning of that region");
    CHECK(report->largest_region_pages == PAGE_CAP,
          "the tracked region was truncated to the cap");
}

static void test_untracked_memory_is_never_handed_out(void) {
    printf("untracked memory is never handed out\n");
    map_main();
    CHECK(pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a map above the page cap initialises");
    const uint64_t covered_end = BIG_BASE + PAGE_CAP * PAGE_SIZE;
    CHECK(covered_end < TAIL_BASE, "the covered prefix ends inside the map");
    CHECK(!pmm_free_page(TAIL_BASE),
          "a page above the covered prefix is not tracked");
    CHECK(!pmm_free_page(covered_end),
          "the first page above the covered prefix is not tracked");
    CHECK(!pmm_free_contiguous_pages(TAIL_BASE, 1U),
          "a contiguous range above the covered prefix is not tracked");
    CHECK(pmm_alloc_contiguous_pages(0U) == 0U,
          "a zero-length contiguous allocation is refused");
    CHECK(pmm_alloc_contiguous_pages(
              (uint32_t)(pmm_free_page_count() + 1U)) == 0U,
          "an allocation larger than the tracked free pages is refused");

    const uint64_t free_pages = pmm_free_page_count();
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(free_pages == report->total_pages - report->bitmap_pages,
          "free pages are the tracked pages minus the bitmap");

    uint64_t allocated = 0U;
    uint64_t count = 0U;
    for (;;) {
        const uint64_t page = pmm_alloc_page();
        if (page == 0U) {
            break;
        }
        allocated = page;
        ++count;
        if (count > PAGE_CAP + 8U) {
            break;
        }
    }
    CHECK(count == free_pages, "the allocator exhausts exactly the tracked pages");
    CHECK(allocated == covered_end - PAGE_SIZE,
          "the last tracked page is handed out last");
    CHECK(allocated < covered_end,
          "no allocation ever leaves the covered prefix");
    CHECK(pmm_alloc_page() == 0U, "an exhausted allocator returns zero");
    CHECK(pmm_free_page(allocated), "the last tracked page can be freed");
    CHECK(pmm_alloc_page() == allocated,
          "the freed tracked page is handed out again");
}

static void test_region_table_overflow_is_published(void) {
    printf("region table overflow is published, not fatal\n");
    map_region_table();
    CHECK(pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a map with more regions than the table holds initialises");
    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->status == PMM_INIT_STATUS_OK, "status is OK");
    CHECK(report->coverage == PMM_COVERAGE_LIMITED,
          "dropped regions make the coverage LIMITED");
    CHECK(report->dropped_regions == TABLE_PIECES - ADMITTED_REGIONS + 1U,
          "the number of dropped usable regions is published");
    CHECK(report->dropped_pages == TABLE_PIECES - ADMITTED_REGIONS + 1U,
          "the dropped pages are published");
    CHECK(report->region_count == ADMITTED_REGIONS,
          "the region table is filled to its documented bound");
    CHECK(report->total_pages == TABLE_SIZE / PAGE_SIZE + ADMITTED_REGIONS - 1U,
          "only the admitted regions are tracked");
    CHECK(report->usable_bytes ==
              TABLE_SIZE + (uint64_t)TABLE_PIECES * TABLE_PIECE_SIZE,
          "the full usable byte count is still reported honestly");
    CHECK(pmm_free_page_count() ==
              report->total_pages - report->bitmap_pages,
          "free pages are the tracked pages minus the bitmap");
    CHECK(!pmm_free_page(TABLE_FIRST_PIECE),
          "a free tracked page is not reported as allocated");
    CHECK(!pmm_free_page(last_dropped_piece_base()),
          "a page in a dropped region is not tracked at all");

    /* Exhausting the allocator proves the dropped regions are never used:
     * exactly the tracked pages are handed out, and no page from a dropped
     * region ever appears. */
    const uint64_t free_pages = pmm_free_page_count();
    uint64_t allocated = 0U;
    uint64_t count = 0U;
    for (;;) {
        const uint64_t page = pmm_alloc_page();
        if (page == 0U) {
            break;
        }
        allocated = page;
        ++count;
        if (count > free_pages + 8U) {
            break;
        }
    }
    CHECK(count == free_pages,
          "the allocator hands out exactly the tracked free pages");
    CHECK(allocated == last_admitted_piece_base(),
          "the last admitted piece is handed out last");
    CHECK(pmm_alloc_page() == 0U,
          "dropped regions are never handed out, even when tracked memory runs out");
}

static void test_failure_reasons_are_specific(void) {
    printf("every failure path leaves its own reason\n");

    CHECK(!pmm_init(NULL, (uint64_t)(uintptr_t)arena),
          "a NULL memory map fails");
    expect_failure("a NULL memory map reports ERR_NO_MEMMAP",
                   PMM_INIT_ERR_NO_MEMMAP);

    map_reset();
    response.entries = NULL;
    response.entry_count = 1U;
    CHECK(!pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a NULL entry array fails");
    expect_failure("a NULL entry array reports ERR_NO_MEMMAP",
                   PMM_INIT_ERR_NO_MEMMAP);

    map_reset();
    CHECK(!pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "an empty memory map fails");
    expect_failure("an empty memory map reports ERR_NO_MEMMAP",
                   PMM_INIT_ERR_NO_MEMMAP);

    map_reset();
    response.entry_count = 1U;
    entry_ptrs[0] = NULL;
    CHECK(!pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a NULL memory map entry fails");
    expect_failure("a NULL entry reports ERR_NULL_ENTRY",
                   PMM_INIT_ERR_NULL_ENTRY);

    map_reset();
    map_add(UINT64_MAX - 8ULL, 16ULL, LIMINE_MEMMAP_USABLE);
    CHECK(!pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a wrapping usable range fails");
    expect_failure("a wrapping range reports ERR_RANGE_OVERFLOW",
                   PMM_INIT_ERR_RANGE_OVERFLOW);

    map_reset();
    map_add(0U, UINT64_MAX / 2U + 1U, LIMINE_MEMMAP_USABLE);
    map_add(0U, UINT64_MAX / 2U + 1U, LIMINE_MEMMAP_USABLE);
    CHECK(!pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "an overflowing usable total fails");
    expect_failure("an overflowing total reports ERR_ARITHMETIC_OVERFLOW",
                   PMM_INIT_ERR_ARITHMETIC_OVERFLOW);

    map_reset();
    map_add(0U, 0x100000ULL, LIMINE_MEMMAP_RESERVED);
    map_add(0x90000ULL, 0x10000ULL, LIMINE_MEMMAP_USABLE);
    CHECK(!pmm_init(&response, (uint64_t)(uintptr_t)arena),
          "a map whose usable memory is all below 1 MiB fails");
    expect_failure("no usable region reports ERR_NO_USABLE_REGION",
                   PMM_INIT_ERR_NO_USABLE_REGION);
}

static void test_reason_tokens_are_distinct(void) {
    printf("failure reason tokens\n");
    const enum pmm_init_status statuses[] = {
        PMM_INIT_STATUS_OK,
        PMM_INIT_ERR_NO_MEMMAP,
        PMM_INIT_ERR_NULL_ENTRY,
        PMM_INIT_ERR_RANGE_OVERFLOW,
        PMM_INIT_ERR_NO_USABLE_REGION,
        PMM_INIT_ERR_NO_BITMAP_PLACEMENT,
        PMM_INIT_ERR_ARITHMETIC_OVERFLOW
    };
    const size_t status_count = sizeof(statuses) / sizeof(statuses[0]);
    for (size_t left = 0U; left < status_count; ++left) {
        CHECK(pmm_init_status_string(statuses[left]) != NULL &&
                  pmm_init_status_string(statuses[left])[0] != '\0',
              "every status has a non-empty token");
        for (size_t right = left + 1U; right < status_count; ++right) {
            CHECK(strcmp(pmm_init_status_string(statuses[left]),
                         pmm_init_status_string(statuses[right])) != 0,
                  "status tokens are distinct");
        }
    }
    CHECK(strcmp(pmm_coverage_string(PMM_COVERAGE_FULL), "FULL") == 0,
          "full coverage is published as FULL");
    CHECK(strcmp(pmm_coverage_string(PMM_COVERAGE_LIMITED), "LIMITED") == 0,
          "limited coverage is published as LIMITED");
    CHECK(strcmp(pmm_coverage_string(PMM_COVERAGE_NONE), "NONE") == 0,
          "missing coverage is published as NONE");
}

int main(void) {
    printf("PMM limited-coverage host test\n");

    arena = mmap(NULL, ARENA_SPAN, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (arena == MAP_FAILED) {
        printf("FAIL: cannot reserve %llu bytes of address space\n",
               (unsigned long long)ARENA_SPAN);
        printf("\n1 FAILURES\n");
        return 1;
    }

    test_over_cap_boot_continues();
    test_untracked_memory_is_never_handed_out();
    test_region_table_overflow_is_published();
    test_failure_reasons_are_specific();
    test_reason_tokens_are_distinct();

    if (munmap(arena, ARENA_SPAN) != 0) {
        printf("FAIL: address space is released\n");
        ++failures;
    }

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("PMM limited-coverage host test: ALL OK");
    return 0;
}

#else
#error "tests/pmm-limited-host.c requires -DHFOC_PMM_LIMITED_HOST_TEST"
#endif
