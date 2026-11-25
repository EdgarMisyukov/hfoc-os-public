#ifndef HFOC_PMM_H
#define HFOC_PMM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "limine_min.h"

/*
 * ITEM-390: one bitmap bit covers one 4 KiB page, so HFOC_PMM_MAX_PAGES is the
 * page count the page bitmap can represent (1 TiB with the production value).
 * The kernel no longer refuses to boot above it: memory beyond the cap is left
 * untracked and published as a LIMITED coverage state instead of a panic.
 * A host proof may lower the cap at compile time to exercise that path with a
 * small synthetic map.
 */
#ifndef HFOC_PMM_MAX_PAGES
#define HFOC_PMM_MAX_PAGES (1U << 28U)
#endif

/* Capacity of the one-line PMM initialisation diagnostic, including the
 * terminating NUL. The renderer truncates instead of overflowing and reports
 * the untruncated length.
 *
 * ITEM-393: the line carries the region summary as well now (first, largest
 * and last region base plus the bitmap span), so a photograph of a machine
 * without a serial console names the shape of the map that produced a
 * physical address the allocator never tracked. 478 -> 704 bytes. */
#define HFOC_PMM_DIAGNOSTIC_MAX 704U

/* Capacity of the one-line page-fault physical classification, including the
 * terminating NUL. */
#define HFOC_PMM_FAULT_LINE_MAX 256U

/* Why the last pmm_init() call failed. Every failure path sets exactly one of
 * these, so the caller can print a specific reason instead of one sentence. */
enum pmm_init_status {
    PMM_INIT_STATUS_UNKNOWN = 0,
    PMM_INIT_STATUS_OK,
    PMM_INIT_ERR_NO_MEMMAP,
    PMM_INIT_ERR_NULL_ENTRY,
    PMM_INIT_ERR_RANGE_OVERFLOW,
    PMM_INIT_ERR_NO_USABLE_REGION,
    PMM_INIT_ERR_NO_BITMAP_PLACEMENT,
    PMM_INIT_ERR_ARITHMETIC_OVERFLOW
};

/* How much of the usable memory in the boot memory map the bitmap covers. */
enum pmm_coverage {
    PMM_COVERAGE_NONE = 0,
    PMM_COVERAGE_FULL,
    PMM_COVERAGE_LIMITED
};

/* What the allocator knows about one physical address.
 *
 * ITEM-393: a write to a physical address the allocator never tracked is a
 * different defect from a write to a page it handed out, and on a machine
 * without a serial console the panic screen is the only witness. This
 * classification is what the page-fault record prints. */
enum pmm_physical_class {
    /* Outside every tracked usable region: firmware hole, MMIO window or
     * reserved address space. The allocator never handed this out and never
     * wrote it. */
    PMM_PHYSICAL_UNTRACKED = 0,
    /* Inside a tracked region but not on a page boundary. */
    PMM_PHYSICAL_UNALIGNED,
    /* A tracked, free page. */
    PMM_PHYSICAL_FREE,
    /* A tracked page the allocator handed out (or a page it holds). */
    PMM_PHYSICAL_ALLOCATED,
    /* Inside the page bitmap itself. */
    PMM_PHYSICAL_BITMAP
};

/* Numbers behind the last pmm_init() call; valid after success and failure.
 * bitmap_phys is zero when no region could hold the bitmap. */
struct pmm_init_diagnostic {
    enum pmm_init_status status;
    enum pmm_coverage coverage;
    uint64_t usable_bytes;
    uint64_t tracked_bytes;
    uint64_t dropped_pages;
    uint64_t total_pages;
    uint64_t page_cap;
    uint32_t region_count;
    uint32_t dropped_regions;
    uint32_t bitmap_pages;
    uint64_t largest_region_pages;
    uint32_t placement_region;
    uint64_t bitmap_phys;
    /* ITEM-393: the region summary. A physical address that is not RAM can
     * only be explained by the map that produced it, so the first, largest
     * and last region bases and the bitmap span travel with the diagnostic. */
    uint64_t first_region_base;
    uint64_t first_region_pages;
    uint64_t largest_region_base;
    uint64_t last_region_base;
    uint64_t bitmap_end_phys;
};

bool pmm_init(struct limine_memmap_response *memory_map, uint64_t hhdm_offset);
const struct pmm_init_diagnostic *pmm_init_diagnostic(void);
const char *pmm_init_status_string(enum pmm_init_status status);
const char *pmm_coverage_string(enum pmm_coverage coverage);
size_t pmm_init_diagnostic_render(
    const struct pmm_init_diagnostic *diagnostic,
    char *buffer,
    size_t size);

/* ITEM-393: where a physical address sits with respect to the tracked usable
 * memory. UNTRACKED means no region contains it, so no allocator path can have
 * produced it and no zeroing path may write it. */
enum pmm_physical_class pmm_classify_physical(uint64_t physical_address);
const char *pmm_physical_class_string(enum pmm_physical_class value);

/* ITEM-393: one bounded line naming a faulting physical address, its class and
 * the region summary that explains it, for the panic screen of a machine
 * without a serial console. Reports the untruncated length like the diagnostic
 * renderer. */
size_t pmm_fault_line(
    uint64_t physical_address,
    char *buffer,
    size_t size);

uint64_t pmm_total_usable_bytes(void);
uint64_t pmm_free_page_count(void);
uint64_t pmm_alloc_page(void);
bool pmm_free_page(uint64_t physical_address);
uint64_t pmm_alloc_contiguous_pages(uint32_t count);
bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count);

#endif
