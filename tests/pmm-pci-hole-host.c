/*
 * ITEM-393 firmware-map / PCI-hole PMM host proof.
 *
 * The production ISO booted on the acceptance host and died in memset with
 * CR2 = 0xFFFF8000FC000000, i.e. a write to physical 0xFC000000 - an address
 * inside the machine's PCI MMIO window (0xC0000000-0xFEC2FFFF), not RAM. The
 * PMM is the one component that writes physical pages it derived from the
 * firmware map, so this proof drives pmm_init() with that machine's map and
 * makes the required property executable rather than asserted:
 *
 *   * the arena stands in for the HHDM, and every span the firmware map does
 *     NOT describe as usable is mapped PROT_NONE. A single byte written
 *     outside usable RAM therefore faults, and the handler reports the exact
 *     physical address and fails the proof - the same failure the hardware
 *     showed, minus the photograph;
 *   * the bitmap span and every page the allocator hands out are checked
 *     against the usable model, and the scaled map is driven to exhaustion so
 *     that "every tracked page is RAM" is proved for the whole tracked set and
 *     not only for a sample;
 *   * a contiguous allocation must stay inside ONE usable region. Bitmap
 *     indices are contiguous across region boundaries, so an index-only scan
 *     hands back a "contiguous" run whose physical span leaves the region and
 *     walks into the firmware hole (or the PCI window) behind it. The scaled
 *     cases below drive exactly that boundary and are why this proof is red
 *     against the unfixed tree.
 *
 * The arena is mapped MAP_NORESERVE: only the bitmap pages and the pages this
 * proof allocates are ever touched, so a multi-gigabyte address span costs a
 * few MiB of committed host memory.
 */
#ifdef HFOC_PMM_PCI_HOLE_HOST_TEST

#include "pmm.h"

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE_SIZE 4096ULL
#define MIB (1024ULL * 1024ULL)
#define GIB (1024ULL * MIB)

/* Address span of the synthetic machine: the host's RAM above 4 GiB plus the
 * firmware block above it. */
#define ADDRESS_SPAN (20ULL * GIB)

/* The acceptance host's RAM, as /proc/iomem reports it. The ellipsis in the
 * report hides the small pieces between the last quoted low range and the PCI
 * window; LOW_TAIL_* stands in for them with the same shape. */
#define LOW_RAM_BASE        UINT64_C(0x000B020000)
#define LOW_RAM_END         UINT64_C(0x099FA8FFF)
#define LOW_RAM_ONE_BASE    UINT64_C(0x099FAA000)
#define LOW_RAM_ONE_END     UINT64_C(0x099FD5FFF)
#define LOW_RAM_TWO_BASE    UINT64_C(0x099FD7000)
#define LOW_RAM_TWO_END     UINT64_C(0x0B4346017)
#define FRAGMENT_BASE       UINT64_C(0x0B4346018)
#define FRAGMENT_END        UINT64_C(0x0B4353857)
#define LOW_TAIL_BASE       UINT64_C(0x0B4354000)
#define LOW_TAIL_END        UINT64_C(0x0BFFFFFFF)

/* The PCI aperture the host reports as "PCI Bus 0000:00", the top-of-4GiB
 * firmware block behind it, and the RAM above 4 GiB. 0xFC000000 - the address
 * the panic wrote - is inside the aperture. */
#define PCI_WINDOW_BASE     UINT64_C(0x0C0000000)
#define PCI_WINDOW_END      UINT64_C(0x0FEC2FFFF)
#define TOP_BLOCK_BASE      UINT64_C(0x0FEC30000)
#define TOP_BLOCK_END       UINT64_C(0x0FFFFFFFF)
#define HIGH_RAM_BASE       UINT64_C(0x100000000)
#define HIGH_RAM_END        UINT64_C(0x41E2FFFFF)
#define HIGH_RAM_REGION_END (HIGH_RAM_BASE + UINT64_C(0x31E300) * 4096ULL)
#define HIGH_TAIL_BASE      UINT64_C(0x41E300000)
#define HIGH_TAIL_END       UINT64_C(0x41EFFFFFF)

/* The address the hardware panic wrote, and the aperture it sits in. */
#define PANIC_PHYSICAL_ADDRESS UINT64_C(0x0FC000000)

/* Low RAM one: the first quoted range, page-trimmed exactly the way the PMM
 * trims a usable entry (0x099FA9000 - 0x0B020000 bytes = 0x8EF89 pages). */
#define LOW_RAM_PAGES UINT64_C(0x8EF89)

struct known_region {
    uint64_t base;
    uint64_t end;
};

/*
 * The usable regions the PMM must derive from the map above: every usable entry
 * page-trimmed (base aligned up, length floored to whole pages), with adjacent
 * ranges coalesced. LOW_TAIL starts a page above the ragged end of FRAGMENT, so
 * the two stay separate - the page between them is not RAM.
 */
static const struct known_region usable_regions[] = {
    { LOW_RAM_BASE, LOW_RAM_BASE + LOW_RAM_PAGES * PAGE_SIZE },
    { LOW_RAM_ONE_BASE, LOW_RAM_ONE_BASE + 44ULL * PAGE_SIZE },
    { LOW_RAM_TWO_BASE, LOW_RAM_TWO_BASE + UINT64_C(0x1A36F) * PAGE_SIZE },
    { UINT64_C(0x0B4347000), UINT64_C(0x0B4353000) },
    { LOW_TAIL_BASE, LOW_TAIL_BASE + UINT64_C(0xBCAC) * PAGE_SIZE },
    { HIGH_RAM_BASE, HIGH_RAM_BASE + UINT64_C(0x31E300) * PAGE_SIZE }
};
#define USABLE_REGION_COUNT \
    (sizeof(usable_regions) / sizeof(usable_regions[0]))

/* The map as the bootloader hands it over: usable ranges plus the reserved
 * ranges that separate them. */
struct map_entry_fixture {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

static const struct map_entry_fixture acceptance_map[] = {
    { 0x000000000ULL, 0x0009F000ULL, LIMINE_MEMMAP_RESERVED },
    { 0x00009F000ULL, 0x00001000ULL, LIMINE_MEMMAP_USABLE },
    { 0x0000A0000ULL, 0x00A00000ULL, LIMINE_MEMMAP_RESERVED },
    { LOW_RAM_BASE, LOW_RAM_END - LOW_RAM_BASE + 1ULL,
      LIMINE_MEMMAP_USABLE },
    { LOW_RAM_END + 1ULL, LOW_RAM_ONE_BASE - LOW_RAM_END - 1ULL,
      LIMINE_MEMMAP_RESERVED },
    { LOW_RAM_ONE_BASE, LOW_RAM_ONE_END - LOW_RAM_ONE_BASE + 1ULL,
      LIMINE_MEMMAP_USABLE },
    { LOW_RAM_ONE_END + 1ULL, LOW_RAM_TWO_BASE - LOW_RAM_ONE_END - 1ULL,
      LIMINE_MEMMAP_RESERVED },
    { LOW_RAM_TWO_BASE, LOW_RAM_TWO_END - LOW_RAM_TWO_BASE + 1ULL,
      LIMINE_MEMMAP_USABLE },
    { FRAGMENT_BASE, FRAGMENT_END - FRAGMENT_BASE + 1ULL,
      LIMINE_MEMMAP_USABLE },
    { LOW_TAIL_BASE, LOW_TAIL_END - LOW_TAIL_BASE + 1ULL,
      LIMINE_MEMMAP_USABLE },
    { PCI_WINDOW_BASE, PCI_WINDOW_END - PCI_WINDOW_BASE + 1ULL,
      LIMINE_MEMMAP_RESERVED },
    { TOP_BLOCK_BASE, TOP_BLOCK_END - TOP_BLOCK_BASE + 1ULL,
      LIMINE_MEMMAP_RESERVED },
    { HIGH_RAM_BASE, HIGH_RAM_END - HIGH_RAM_BASE + 1ULL,
      LIMINE_MEMMAP_USABLE },
    { HIGH_TAIL_BASE, HIGH_TAIL_END - HIGH_TAIL_BASE + 1ULL,
      LIMINE_MEMMAP_RESERVED }
};
#define ACCEPTANCE_MAP_COUNT \
    (sizeof(acceptance_map) / sizeof(acceptance_map[0]))

/*
 * The scaled map: the same shape in a few pages, so the region-boundary cases
 * that need an exhausted region are reachable in a handful of allocations.
 *
 *   A  [0x0B020000, 0x0B022000)   8 pages  (trimmed from a ragged entry)
 *   B  [0x0B024000, 0x0B028000)   4 pages
 *      firmware hole              2 pages
 *   C  [0x0B02A000, 0x0B02D000)   3 pages
 *      reserved                   up to the aperture
 *      PCI aperture               the host's shape
 *   D  [0x100000000, 0x100020000) 32 pages above 4 GiB - the largest region,
 *                                 so the bitmap is placed there, as on the
 *                                 host
 */
/* Ragged tail of region A's firmware entry: the last 0x800 bytes are not a
 * whole page, so the PMM must floor the range instead of tracking them. */
#define SCALED_A_RAGGED 0x800ULL

#define SCALED_A_BASE   UINT64_C(0x0B020000)
#define SCALED_A_PAGES  8U
#define SCALED_A_END    (SCALED_A_BASE + (uint64_t)SCALED_A_PAGES * PAGE_SIZE)
#define SCALED_B_BASE   (SCALED_A_END + 2ULL * PAGE_SIZE)
#define SCALED_B_PAGES  4U
#define SCALED_B_END    (SCALED_B_BASE + (uint64_t)SCALED_B_PAGES * PAGE_SIZE)
#define SCALED_C_BASE   (SCALED_B_END + 2ULL * PAGE_SIZE)
#define SCALED_C_PAGES  3U
#define SCALED_C_END    (SCALED_C_BASE + (uint64_t)SCALED_C_PAGES * PAGE_SIZE)
#define SCALED_D_BASE   UINT64_C(0x100000000)
#define SCALED_D_PAGES  32U
#define SCALED_D_END    (SCALED_D_BASE + (uint64_t)SCALED_D_PAGES * PAGE_SIZE)
#define SCALED_TOTAL_PAGES \
    ((uint64_t)SCALED_A_PAGES + SCALED_B_PAGES + SCALED_C_PAGES + \
     SCALED_D_PAGES)

static const struct known_region scaled_regions[] = {
    { SCALED_A_BASE, SCALED_A_END },
    { SCALED_B_BASE, SCALED_B_END },
    { SCALED_C_BASE, SCALED_C_END },
    { SCALED_D_BASE, SCALED_D_END }
};
#define SCALED_REGION_COUNT \
    (sizeof(scaled_regions) / sizeof(scaled_regions[0]))

static const struct map_entry_fixture scaled_map[] = {
    { 0x000000000ULL, 0x0009F000ULL, LIMINE_MEMMAP_RESERVED },
    { 0x00009F000ULL, 0x00001000ULL, LIMINE_MEMMAP_USABLE },
    { 0x0000A0000ULL, 0x00A00000ULL, LIMINE_MEMMAP_RESERVED },
    /* A usable entry whose length is not a whole number of pages. */
    { SCALED_A_BASE, SCALED_A_END - SCALED_A_BASE + SCALED_A_RAGGED,
      LIMINE_MEMMAP_USABLE },
    { SCALED_A_END + SCALED_A_RAGGED, SCALED_B_BASE - SCALED_A_END -
          SCALED_A_RAGGED,
      LIMINE_MEMMAP_RESERVED },
    { SCALED_B_BASE, SCALED_B_END - SCALED_B_BASE, LIMINE_MEMMAP_USABLE },
    { SCALED_B_END, SCALED_C_BASE - SCALED_B_END, LIMINE_MEMMAP_RESERVED },
    { SCALED_C_BASE, SCALED_C_END - SCALED_C_BASE, LIMINE_MEMMAP_USABLE },
    { SCALED_C_END, PCI_WINDOW_BASE - SCALED_C_END, LIMINE_MEMMAP_RESERVED },
    { PCI_WINDOW_BASE, PCI_WINDOW_END - PCI_WINDOW_BASE + 1ULL,
      LIMINE_MEMMAP_RESERVED },
    { TOP_BLOCK_BASE, TOP_BLOCK_END - TOP_BLOCK_BASE + 1ULL,
      LIMINE_MEMMAP_RESERVED },
    { SCALED_D_BASE, SCALED_D_END - SCALED_D_BASE, LIMINE_MEMMAP_USABLE }
};
#define SCALED_MAP_COUNT (sizeof(scaled_map) / sizeof(scaled_map[0]))

static struct limine_memmap_entry entries[32];
static struct limine_memmap_entry *entry_ptrs[32];
static struct limine_memmap_response response;
static unsigned char *arena;
static int failures;
static volatile int guard_fired;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        printf("FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

/*
 * The arena is the HHDM: address X of the arena is physical X. Usable spans are
 * writable, everything else is PROT_NONE, so a write the PMM derives from a
 * region that is not RAM stops here with the physical address that caused it
 * instead of quietly corrupting the host test.
 */
static void guard_handler(int signal_number, siginfo_t *info, void *context) {
    (void)signal_number;
    (void)context;
    const uint64_t address = (uint64_t)(uintptr_t)info->si_addr;
    const uint64_t physical =
        address >= (uint64_t)(uintptr_t)arena
            ? address - (uint64_t)(uintptr_t)arena
            : address;
    guard_fired = 1;
    printf("FAIL: the PMM wrote physical 0x%llX, which the firmware map does "
           "not describe as usable RAM\n",
           (unsigned long long)physical);
    fflush(stdout);
    _exit(1);
}

static void install_guard(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = guard_handler;
    action.sa_flags = SA_SIGINFO;
    if (sigaction(SIGSEGV, &action, NULL) != 0 ||
        sigaction(SIGBUS, &action, NULL) != 0) {
        printf("FAIL: cannot install the usable-RAM guard\n");
        _exit(1);
    }
}

static void make_usable(const struct known_region *region) {
    const uint64_t base = region->base & ~(PAGE_SIZE - 1ULL);
    const uint64_t end = (region->end + PAGE_SIZE - 1ULL) & ~(PAGE_SIZE - 1ULL);
    if (mprotect(arena + base, (size_t)(end - base),
                 PROT_READ | PROT_WRITE) != 0) {
        printf("FAIL: cannot arm the guard for 0x%llX..0x%llX\n",
               (unsigned long long)base, (unsigned long long)end);
        _exit(1);
    }
}

/*
 * The two fixtures overlap in address space (both model the host's low RAM), so
 * the writable scope is re-armed per fixture: everything is PROT_NONE again and
 * only the fixture's own usable ranges are made writable. A write into the
 * other fixture's hole - or into this one's - therefore faults, and the fault
 * address is the physical address the PMM used.
 */
static void arm_scope(const struct known_region *regions, size_t region_count) {
    if (mprotect(arena, (size_t)ADDRESS_SPAN, PROT_NONE) != 0) {
        printf("FAIL: cannot reset the usable-RAM guard\n");
        _exit(1);
    }
    for (size_t index = 0U; index < region_count; ++index) {
        make_usable(&regions[index]);
    }
}

static bool inside_regions(const struct known_region *regions,
                           size_t region_count,
                           uint64_t physical) {
    for (size_t index = 0U; index < region_count; ++index) {
        if (physical >= regions[index].base && physical < regions[index].end) {
            return true;
        }
    }
    return false;
}

static uint64_t region_end_of(const struct known_region *regions,
                              size_t region_count,
                              uint64_t physical) {
    for (size_t index = 0U; index < region_count; ++index) {
        if (physical >= regions[index].base && physical < regions[index].end) {
            return regions[index].end;
        }
    }
    return 0U;
}

static bool inside_pci_window(uint64_t physical) {
    return physical >= PCI_WINDOW_BASE && physical <= PCI_WINDOW_END;
}

static bool build_map(const struct map_entry_fixture *fixture,
                      size_t count) {
    if (count > sizeof(entries) / sizeof(entries[0])) {
        return false;
    }
    memset(entries, 0, sizeof(entries));
    memset(entry_ptrs, 0, sizeof(entry_ptrs));
    for (size_t index = 0U; index < count; ++index) {
        entries[index].base = fixture[index].base;
        entries[index].length = fixture[index].length;
        entries[index].type = fixture[index].type;
        entry_ptrs[index] = &entries[index];
    }
    response.revision = 0U;
    response.entry_count = count;
    response.entries = entry_ptrs;
    return true;
}

static bool initialise(const struct map_entry_fixture *fixture,
                       size_t count) {
    return build_map(fixture, count) &&
           pmm_init(&response, (uint64_t)(uintptr_t)arena);
}

/* ---------------------------------------------------------- acceptance map - */

static void test_acceptance_map_initialises(void) {
    printf("the acceptance host's map initialises\n");

    CHECK(build_map(acceptance_map, ACCEPTANCE_MAP_COUNT),
          "the acceptance map fixture fits");
    CHECK(initialise(acceptance_map, ACCEPTANCE_MAP_COUNT),
          "the acceptance host's firmware map initialises");

    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->status == PMM_INIT_STATUS_OK, "status is OK");
    CHECK(report->coverage == PMM_COVERAGE_FULL,
          "coverage is FULL: the fragmented map drops nothing");
    CHECK(report->dropped_regions == 0U && report->dropped_pages == 0U,
          "no usable range of the map is dropped");
    CHECK(report->region_count == USABLE_REGION_COUNT,
          "the region set is exactly the usable set of the firmware map");
    CHECK(report->usable_bytes > 4ULL * GIB,
          "the machine presents its multi-gigabyte usable memory");
    CHECK(report->tracked_bytes == report->total_pages * PAGE_SIZE,
          "tracked bytes match the tracked page count");
}

static void test_acceptance_map_bitmap_is_ram(void) {
    printf("the bitmap lands in RAM, above the PCI aperture\n");

    CHECK(initialise(acceptance_map, ACCEPTANCE_MAP_COUNT),
          "the acceptance host's firmware map initialises");

    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->bitmap_pages > 0U, "the bitmap reserves pages");
    CHECK(inside_regions(usable_regions, USABLE_REGION_COUNT,
                         report->bitmap_phys),
          "the first bitmap page is inside a usable region");
    CHECK(!inside_pci_window(report->bitmap_phys),
          "the bitmap is not inside the PCI aperture");
    CHECK(report->bitmap_phys == HIGH_RAM_BASE,
          "the bitmap is taken from the largest region, above 4 GiB");
    CHECK(inside_regions(usable_regions, USABLE_REGION_COUNT,
                         report->bitmap_end_phys - 1ULL),
          "the last bitmap page is inside the same usable region");
    CHECK(report->bitmap_end_phys <=
              region_end_of(usable_regions, USABLE_REGION_COUNT,
                            report->bitmap_phys),
          "the bitmap stays inside the region it was taken from");
    CHECK(report->largest_region_base == HIGH_RAM_BASE,
          "the published largest region base is the above-4GiB region");
    CHECK(report->first_region_base == LOW_RAM_BASE,
          "the published first region base is the lowest usable range");
    CHECK(report->first_region_pages == LOW_RAM_PAGES,
          "the published first region page count is the trimmed range");
    CHECK(report->last_region_base == HIGH_RAM_BASE,
          "the published last region base is the highest usable range");
}

static void test_every_tracked_page_is_ram(void) {
    printf("no allocation leaves usable RAM\n");

    CHECK(initialise(acceptance_map, ACCEPTANCE_MAP_COUNT),
          "the acceptance host's firmware map initialises");

    uint64_t allocator_pages = 0U;
    uint64_t outside = 0U;
    uint64_t in_aperture = 0U;
    for (uint64_t index = 0U; index < 8192U; ++index) {
        const uint64_t physical = pmm_alloc_page();
        if (physical == 0U) {
            break;
        }
        ++allocator_pages;
        if (!inside_regions(usable_regions, USABLE_REGION_COUNT, physical)) {
            ++outside;
        }
        if (inside_pci_window(physical)) {
            ++in_aperture;
        }
    }
    CHECK(allocator_pages == 8192U, "the allocator hands out pages steadily");
    CHECK(outside == 0U, "every allocated page is inside a usable region");
    CHECK(in_aperture == 0U, "no allocated page is inside the PCI aperture");

    /* The panic address itself: the aperture the hardware faulted in must be
     * something the allocator can never produce or write. */
    CHECK(!inside_regions(usable_regions, USABLE_REGION_COUNT,
                          PANIC_PHYSICAL_ADDRESS),
          "the address the hardware panic wrote is not RAM on this map");
    CHECK(pmm_classify_physical(PANIC_PHYSICAL_ADDRESS) ==
              PMM_PHYSICAL_UNTRACKED,
          "the PMM classifies the panic address as untracked");
    CHECK(pmm_classify_physical(PCI_WINDOW_BASE) == PMM_PHYSICAL_UNTRACKED,
          "the PMM classifies the start of the aperture as untracked");
    CHECK(pmm_classify_physical(HIGH_RAM_REGION_END) ==
              PMM_PHYSICAL_UNTRACKED,
          "the PMM classifies the first address above RAM as untracked");
    CHECK(pmm_classify_physical(LOW_RAM_TWO_BASE + 1ULL) ==
              PMM_PHYSICAL_UNALIGNED,
          "the PMM classifies a ragged in-region address as unaligned");
}

static void test_fault_line_names_the_address(void) {
    printf("the page-fault record names the failing physical address\n");

    CHECK(initialise(acceptance_map, ACCEPTANCE_MAP_COUNT),
          "the acceptance host's firmware map initialises");

    char line[HFOC_PMM_FAULT_LINE_MAX];
    const size_t written =
        pmm_fault_line(PANIC_PHYSICAL_ADDRESS, line, sizeof(line));
    CHECK(written > 0U && written + 1U <= sizeof(line),
          "the page-fault record fits its documented bound");
    CHECK(strstr(line, "phys=0x00000000FC000000") == line,
          "the record starts with the faulting physical address");
    CHECK(strstr(line, "class=UNTRACKED") != NULL,
          "the record names the class: the PMM never tracked it");
    CHECK(strstr(line, "region=none") != NULL,
          "the record states that no region contains it");
    CHECK(strstr(line, "bitmap_phys=") != NULL,
          "the record carries the bitmap placement for the photograph");

    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    char bitmap_line[HFOC_PMM_FAULT_LINE_MAX];
    (void)pmm_fault_line(report->bitmap_phys, bitmap_line,
                         sizeof(bitmap_line));
    CHECK(strstr(bitmap_line, "class=BITMAP") != NULL,
          "the bitmap itself is classified as the bitmap");

    char small[16];
    const size_t small_written =
        pmm_fault_line(PANIC_PHYSICAL_ADDRESS, small, sizeof(small));
    CHECK(small_written == written,
          "truncated rendering still reports the untruncated length");
    CHECK(small[sizeof(small) - 1U] == '\0',
          "truncated rendering still terminates the buffer");
}

/* -------------------------------------------------------------- scaled map - */

static uint64_t scaled_fill_until_free_pages(uint64_t remaining_free) {
    uint64_t allocated = 0U;
    while (pmm_free_page_count() > remaining_free) {
        if (pmm_alloc_page() == 0U) {
            break;
        }
        ++allocated;
    }
    return allocated;
}

static void test_contiguous_run_stays_in_one_region(void) {
    printf("a contiguous run stays inside one usable region\n");

    CHECK(initialise(scaled_map, SCALED_MAP_COUNT),
          "the scaled acceptance-shaped map initialises");

    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    CHECK(report->region_count == SCALED_REGION_COUNT,
          "the scaled map has one region per usable range");
    CHECK(report->bitmap_phys == SCALED_D_BASE,
          "the scaled bitmap is placed above the aperture, in the largest "
          "region");

    /* Fill the first region except its last page, so a multi-page run would
     * have to start on that page and continue into the next region. */
    const uint64_t free_before = pmm_free_page_count();
    const uint64_t filled = scaled_fill_until_free_pages(
        free_before - (SCALED_A_PAGES - 1U));
    CHECK(filled == SCALED_A_PAGES - 1U,
          "the first region is filled except its last page");

    const uint64_t run = pmm_alloc_contiguous_pages(SCALED_B_PAGES);
    CHECK(run != 0U, "a four-page run is served");
    if (run != 0U) {
        const uint64_t run_end = run + (uint64_t)SCALED_B_PAGES * PAGE_SIZE;
        const uint64_t region_end =
            region_end_of(scaled_regions, SCALED_REGION_COUNT, run);
        CHECK(region_end != 0U,
              "the whole run lies inside one usable region");
        CHECK(run_end <= region_end,
              "the run's physical span stays inside that region");
        CHECK(run == SCALED_B_BASE,
              "the run comes from the first region that can hold it, not from "
              "the tail of the exhausted one");
        CHECK(!inside_pci_window(run) && !inside_pci_window(run_end - 1ULL),
              "no page of the run is inside the PCI aperture");
    }
}

static void test_contiguous_run_refuses_instead_of_straddling(void) {
    printf("a request no single region can serve fails closed\n");

    CHECK(initialise(scaled_map, SCALED_MAP_COUNT),
          "the scaled acceptance-shaped map initialises");

    /* Leave two free pages in the first region and consume the second region
     * entirely, so every remaining candidate run is smaller than the request:
     * A holds two pages, C three and D (minus its bitmap page) thirty-one. */
    const uint64_t free_before = pmm_free_page_count();
    const uint64_t filled = scaled_fill_until_free_pages(
        free_before - (SCALED_A_PAGES - 2U + SCALED_B_PAGES));
    CHECK(filled == SCALED_A_PAGES - 2U + SCALED_B_PAGES,
          "the first two regions are reduced to two free pages");

    const uint64_t request = SCALED_D_PAGES + 1U;
    const uint64_t run = pmm_alloc_contiguous_pages((uint32_t)request);
    if (run != 0U) {
        const uint64_t region_end =
            region_end_of(scaled_regions, SCALED_REGION_COUNT, run);
        printf("FAIL: a %llu-page run was served from 0x%llX although no "
               "region holds that many pages\n",
               (unsigned long long)request, (unsigned long long)run);
        ++failures;
        CHECK(region_end != 0U &&
                  run + request * PAGE_SIZE <= region_end,
              "the served run is physically contiguous inside one region");
        (void)pmm_free_contiguous_pages(run, (uint32_t)request);
    } else {
        printf("PASS: the request fails instead of returning a run that "
               "leaves its region\n");
    }
}

static void test_exhaustion_never_leaves_usable_ram(void) {
    printf("the whole tracked set is usable RAM\n");

    CHECK(initialise(scaled_map, SCALED_MAP_COUNT),
          "the scaled acceptance-shaped map initialises");

    const struct pmm_init_diagnostic *report = pmm_init_diagnostic();
    const uint64_t expected_free =
        SCALED_TOTAL_PAGES - (uint64_t)report->bitmap_pages;
    CHECK(pmm_free_page_count() == expected_free,
          "every tracked page except the bitmap starts free");

    uint64_t allocated = 0U;
    uint64_t outside = 0U;
    uint64_t in_bitmap = 0U;
    for (;;) {
        const uint64_t physical = pmm_alloc_page();
        if (physical == 0U) {
            break;
        }
        ++allocated;
        if (!inside_regions(scaled_regions, SCALED_REGION_COUNT, physical)) {
            ++outside;
        }
        if (physical >= report->bitmap_phys &&
            physical < report->bitmap_end_phys) {
            ++in_bitmap;
        }
    }
    CHECK(allocated == expected_free,
          "an exhausted allocator hands out exactly the tracked free pages");
    CHECK(outside == 0U,
          "no page of the whole tracked set lies outside usable RAM");
    CHECK(in_bitmap == 0U, "no bitmap page is ever handed out");
    CHECK(pmm_free_page_count() == 0U, "the free count reaches zero");
    CHECK(pmm_alloc_page() == 0U, "an exhausted allocator fails closed");
}

int main(void) {
    printf("PMM firmware-map / PCI-hole host test\n");

    arena = mmap(NULL, (size_t)ADDRESS_SPAN, PROT_NONE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (arena == MAP_FAILED) {
        printf("FAIL: cannot reserve %llu bytes of address space\n",
               (unsigned long long)ADDRESS_SPAN);
        printf("\n1 FAILURES\n");
        return 1;
    }
    install_guard();
    arm_scope(usable_regions, USABLE_REGION_COUNT);

    test_acceptance_map_initialises();
    test_acceptance_map_bitmap_is_ram();
    test_every_tracked_page_is_ram();
    test_fault_line_names_the_address();

    arm_scope(scaled_regions, SCALED_REGION_COUNT);

    test_contiguous_run_stays_in_one_region();
    test_contiguous_run_refuses_instead_of_straddling();
    test_exhaustion_never_leaves_usable_ram();

    if (guard_fired != 0) {
        printf("FAIL: the usable-RAM guard caught a write outside RAM\n");
        ++failures;
    }

    if (munmap(arena, (size_t)ADDRESS_SPAN) != 0) {
        printf("FAIL: address space is released\n");
        ++failures;
    }

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("PMM firmware-map / PCI-hole host test: ALL OK");
    return 0;
}

#else
#error "tests/pmm-pci-hole-host.c requires -DHFOC_PMM_PCI_HOLE_HOST_TEST"
#endif
