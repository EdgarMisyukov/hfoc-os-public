#include "pmm.h"
#include "kstring.h"
#include <stddef.h>

#define PAGE_SIZE 4096ULL
#define LOW_MEMORY_LIMIT 0x100000ULL
/*
 * ITEM-390: the region table used to be a hard 64-entry limit that failed the
 * whole boot. It is larger now, and a map that still overflows it drops the
 * extra usable ranges and publishes how much memory was dropped instead of
 * refusing to boot.
 */
#define MAX_REGIONS 256U

static uint64_t direct_map_offset;
static uint64_t total_usable;
static uint64_t total_page_count;
static uint64_t free_page_count_value;
static uint64_t *page_bitmap;
static uint64_t bitmap_word_count;
static uint64_t bitmap_phys;
static uint32_t bitmap_page_count;
static bool pmm_ready;

struct pmm_region {
    uint64_t base;
    uint64_t end;
    uint64_t start_page;
    uint64_t page_count;
};

static struct pmm_region regions[MAX_REGIONS];
static uint32_t region_count;
static uint32_t dropped_region_count;
static uint64_t dropped_region_pages;
static uint64_t cap_dropped_pages;
static uint32_t bitmap_region_index;
static struct pmm_init_diagnostic diagnostic;

static uint64_t align_up(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

static void diagnostic_update(void) {
    uint64_t largest_region_pages = 0U;
    uint32_t largest_index = 0U;
    for (uint32_t index = 0U; index < region_count; ++index) {
        if (regions[index].page_count > largest_region_pages) {
            largest_region_pages = regions[index].page_count;
            largest_index = index;
        }
    }
    diagnostic.total_pages = total_page_count;
    diagnostic.page_cap = (uint64_t)HFOC_PMM_MAX_PAGES;
    diagnostic.usable_bytes = total_usable;
    diagnostic.tracked_bytes = total_page_count * PAGE_SIZE;
    diagnostic.dropped_pages = cap_dropped_pages + dropped_region_pages;
    diagnostic.region_count = region_count;
    diagnostic.dropped_regions = dropped_region_count;
    diagnostic.bitmap_pages = bitmap_page_count;
    diagnostic.largest_region_pages = largest_region_pages;
    diagnostic.placement_region = bitmap_region_index;
    diagnostic.bitmap_phys = bitmap_phys;
    /*
     * ITEM-393: the region summary. When a physical address turns out not to
     * be RAM, these are the numbers that explain which map produced it.
     */
    diagnostic.first_region_base =
        region_count > 0U ? regions[0].base : 0U;
    diagnostic.first_region_pages =
        region_count > 0U ? regions[0].page_count : 0U;
    diagnostic.largest_region_base =
        region_count > 0U ? regions[largest_index].base : 0U;
    diagnostic.last_region_base =
        region_count > 0U ? regions[region_count - 1U].base : 0U;
    diagnostic.bitmap_end_phys =
        bitmap_phys + (uint64_t)bitmap_page_count * PAGE_SIZE;
}

static bool pmm_fail(enum pmm_init_status status) {
    diagnostic.status = status;
    diagnostic.coverage = PMM_COVERAGE_NONE;
    pmm_ready = false;
    diagnostic_update();
    return false;
}

/*
 * Admit one usable, page-aligned physical range. Overlapping and adjacent
 * ranges coalesce, so a firmware map that reports the same memory twice (or in
 * many touching pieces) cannot inflate the page count past the cap.
 * Returns false only when the region table is full; the caller accounts for
 * the dropped range instead of failing the boot.
 */
static bool add_region(uint64_t base, uint64_t end) {
    if (base >= end) {
        return true;
    }
    if (region_count >= MAX_REGIONS) {
        return false;
    }
    regions[region_count].base = base;
    regions[region_count].end = end;
    regions[region_count].start_page = 0U;
    regions[region_count].page_count = (end - base) / PAGE_SIZE;
    ++region_count;

    for (uint32_t index = region_count - 1U; index > 0U; --index) {
        if (regions[index - 1U].base <= regions[index].base) {
            break;
        }
        const struct pmm_region temporary = regions[index - 1U];
        regions[index - 1U] = regions[index];
        regions[index] = temporary;
    }

    uint32_t write_index = 0U;
    for (uint32_t index = 0U; index < region_count; ++index) {
        if (write_index > 0U &&
            regions[write_index - 1U].end >= regions[index].base) {
            if (regions[index].end > regions[write_index - 1U].end) {
                regions[write_index - 1U].end = regions[index].end;
            }
            regions[write_index - 1U].page_count =
                (regions[write_index - 1U].end -
                 regions[write_index - 1U].base) / PAGE_SIZE;
            continue;
        }
        if (write_index != index) {
            regions[write_index] = regions[index];
        }
        ++write_index;
    }
    region_count = write_index;
    return true;
}

static void *phys_to_virt(uint64_t physical) {
    return (void *)(uintptr_t)(physical + direct_map_offset);
}

static void zero_page(uint64_t physical) {
    memset(phys_to_virt(physical), 0, PAGE_SIZE);
}

static bool phys_to_index(uint64_t physical, uint64_t *out_index) {
    if ((physical & (PAGE_SIZE - 1U)) != 0U) {
        return false;
    }
    for (uint32_t index = 0U; index < region_count; ++index) {
        const struct pmm_region *region = &regions[index];
        if (physical >= region->base && physical < region->end) {
            *out_index =
                region->start_page + (physical - region->base) / PAGE_SIZE;
            return true;
        }
    }
    return false;
}

static uint64_t index_to_phys(uint64_t index) {
    for (uint32_t region_index = 0U; region_index < region_count;
         ++region_index) {
        const struct pmm_region *region = &regions[region_index];
        if (index < region->page_count) {
            return region->base + index * PAGE_SIZE;
        }
        index -= region->page_count;
    }
    return 0U;
}

static bool bit_allocated(uint64_t index) {
    return (page_bitmap[index / 64U] &
            (1ULL << (index % 64U))) != 0U;
}

static void set_bit_allocated(uint64_t index) {
    page_bitmap[index / 64U] |= (1ULL << (index % 64U));
}

static void clear_bit_allocated(uint64_t index) {
    page_bitmap[index / 64U] &= ~(1ULL << (index % 64U));
}

static bool range_touches_bitmap(uint64_t start, uint64_t end) {
    if (bitmap_page_count == 0U) {
        return false;
    }
    const uint64_t bitmap_end =
        bitmap_phys + (uint64_t)bitmap_page_count * PAGE_SIZE;
    return start < bitmap_end && bitmap_phys < end;
}

bool pmm_init(struct limine_memmap_response *memory_map, uint64_t hhdm_offset) {
    pmm_ready = false;
    page_bitmap = NULL;
    bitmap_word_count = 0U;
    bitmap_phys = 0U;
    bitmap_page_count = 0U;
    total_page_count = 0U;
    free_page_count_value = 0U;
    region_count = 0U;
    dropped_region_count = 0U;
    dropped_region_pages = 0U;
    cap_dropped_pages = 0U;
    bitmap_region_index = 0U;
    diagnostic.status = PMM_INIT_STATUS_UNKNOWN;
    diagnostic.coverage = PMM_COVERAGE_NONE;
    total_usable = 0U;
    diagnostic_update();

    if (memory_map == NULL || memory_map->entries == NULL ||
        memory_map->entry_count == 0) {
        return pmm_fail(PMM_INIT_ERR_NO_MEMMAP);
    }
    direct_map_offset = hhdm_offset;

    for (uint64_t index = 0; index < memory_map->entry_count; ++index) {
        const struct limine_memmap_entry *entry = memory_map->entries[index];
        if (entry == NULL) {
            return pmm_fail(PMM_INIT_ERR_NULL_ENTRY);
        }
        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }
        if (entry->base > UINT64_MAX - entry->length) {
            return pmm_fail(PMM_INIT_ERR_RANGE_OVERFLOW);
        }
        if (total_usable > UINT64_MAX - entry->length) {
            return pmm_fail(PMM_INIT_ERR_ARITHMETIC_OVERFLOW);
        }
        total_usable += entry->length;

        const uint64_t entry_end = entry->base + entry->length;
        if (entry_end <= LOW_MEMORY_LIMIT) {
            continue;
        }

        uint64_t region_base = entry->base;
        if (region_base < LOW_MEMORY_LIMIT) {
            region_base = LOW_MEMORY_LIMIT;
        }
        if (region_base > UINT64_MAX - (PAGE_SIZE - 1U)) {
            continue;
        }
        region_base = align_up(region_base, PAGE_SIZE);
        if (region_base >= entry_end) {
            continue;
        }

        const uint64_t full_pages =
            (entry_end - region_base) / PAGE_SIZE;
        if (full_pages == 0U) {
            continue;
        }
        const uint64_t region_end =
            region_base + full_pages * PAGE_SIZE;
        if (!add_region(region_base, region_end)) {
            ++dropped_region_count;
            if (dropped_region_pages > UINT64_MAX - full_pages) {
                dropped_region_pages = UINT64_MAX;
            } else {
                dropped_region_pages += full_pages;
            }
        }
    }

    if (region_count == 0U || total_usable < PAGE_SIZE) {
        return pmm_fail(PMM_INIT_ERR_NO_USABLE_REGION);
    }

    uint64_t cumulative_pages = 0U;
    for (uint32_t index = 0U; index < region_count; ++index) {
        regions[index].start_page = cumulative_pages;
        if (cumulative_pages > UINT64_MAX - regions[index].page_count) {
            return pmm_fail(PMM_INIT_ERR_ARITHMETIC_OVERFLOW);
        }
        cumulative_pages += regions[index].page_count;
    }

    /*
     * ITEM-390: a machine with more RAM than the bitmap can cover keeps
     * booting. The covered prefix of the (ascending) region list is tracked;
     * the rest is dropped and published as dropped_pages/LIMITED coverage.
     */
    uint64_t covered_pages = cumulative_pages;
    if (covered_pages > (uint64_t)HFOC_PMM_MAX_PAGES) {
        covered_pages = (uint64_t)HFOC_PMM_MAX_PAGES;
        cap_dropped_pages = cumulative_pages - covered_pages;
    }

    uint64_t running_pages = 0U;
    for (uint32_t index = 0U; index < region_count; ++index) {
        if (running_pages >= covered_pages) {
            region_count = index;
            break;
        }
        const uint64_t remaining_pages = covered_pages - running_pages;
        if (regions[index].page_count > remaining_pages) {
            regions[index].page_count = remaining_pages;
            regions[index].end =
                regions[index].base + remaining_pages * PAGE_SIZE;
            region_count = index + 1U;
            break;
        }
        running_pages += regions[index].page_count;
    }
    total_page_count = covered_pages;

    bitmap_word_count = (total_page_count + 63U) / 64U;
    if (bitmap_word_count > UINT64_MAX / sizeof(uint64_t)) {
        return pmm_fail(PMM_INIT_ERR_ARITHMETIC_OVERFLOW);
    }
    const uint64_t bitmap_byte_count = bitmap_word_count * sizeof(uint64_t);
    const uint64_t bitmap_page_count_wide =
        (bitmap_byte_count + PAGE_SIZE - 1U) / PAGE_SIZE;
    if (bitmap_page_count_wide == 0U ||
        bitmap_page_count_wide > UINT32_MAX) {
        return pmm_fail(PMM_INIT_ERR_ARITHMETIC_OVERFLOW);
    }
    bitmap_page_count = (uint32_t)bitmap_page_count_wide;

    /*
     * ITEM-390: the bitmap used to be taken from regions[0] unconditionally,
     * which fails a fragmented map whose lowest usable range is smaller than
     * the bitmap. Place it in the largest region that can actually hold it, and
     * keep it inside that region.
     */
    uint32_t placement = MAX_REGIONS;
    for (uint32_t index = 0U; index < region_count; ++index) {
        if (regions[index].page_count < bitmap_page_count) {
            continue;
        }
        if (placement == MAX_REGIONS ||
            regions[index].page_count > regions[placement].page_count) {
            placement = index;
        }
    }
    if (placement == MAX_REGIONS) {
        return pmm_fail(PMM_INIT_ERR_NO_BITMAP_PLACEMENT);
    }
    bitmap_region_index = placement;
    bitmap_phys = regions[placement].base;

    uint64_t bitmap_start_index = 0U;
    if (!phys_to_index(bitmap_phys, &bitmap_start_index)) {
        return pmm_fail(PMM_INIT_ERR_NO_BITMAP_PLACEMENT);
    }
    if (bitmap_start_index + (uint64_t)bitmap_page_count > total_page_count) {
        return pmm_fail(PMM_INIT_ERR_NO_BITMAP_PLACEMENT);
    }

    for (uint32_t index = 0U; index < bitmap_page_count; ++index) {
        zero_page(bitmap_phys + (uint64_t)index * PAGE_SIZE);
    }
    page_bitmap = (uint64_t *)phys_to_virt(bitmap_phys);
    for (uint64_t index = 0U; index < bitmap_word_count; ++index) {
        page_bitmap[index] = ~0ULL;
    }

    free_page_count_value = 0U;
    for (uint32_t region_index = 0U; region_index < region_count;
         ++region_index) {
        const struct pmm_region *region = &regions[region_index];
        for (uint64_t page_offset = 0U; page_offset < region->page_count;
             ++page_offset) {
            const uint64_t index = region->start_page + page_offset;
            if (bit_allocated(index)) {
                clear_bit_allocated(index);
                ++free_page_count_value;
            }
        }
    }

    for (uint32_t index = 0U; index < bitmap_page_count; ++index) {
        const uint64_t bitmap_index = bitmap_start_index + index;
        if (!bit_allocated(bitmap_index)) {
            set_bit_allocated(bitmap_index);
            --free_page_count_value;
        }
    }

    diagnostic.status = PMM_INIT_STATUS_OK;
    diagnostic.coverage =
        (cap_dropped_pages == 0U && dropped_region_count == 0U)
            ? PMM_COVERAGE_FULL
            : PMM_COVERAGE_LIMITED;
    pmm_ready = true;
    diagnostic_update();
    return true;
}

const struct pmm_init_diagnostic *pmm_init_diagnostic(void) {
    return &diagnostic;
}

const char *pmm_init_status_string(enum pmm_init_status status) {
    switch (status) {
    case PMM_INIT_STATUS_OK:
        return "OK";
    case PMM_INIT_ERR_NO_MEMMAP:
        return "ERR_NO_MEMMAP";
    case PMM_INIT_ERR_NULL_ENTRY:
        return "ERR_NULL_ENTRY";
    case PMM_INIT_ERR_RANGE_OVERFLOW:
        return "ERR_RANGE_OVERFLOW";
    case PMM_INIT_ERR_NO_USABLE_REGION:
        return "ERR_NO_USABLE_REGION";
    case PMM_INIT_ERR_NO_BITMAP_PLACEMENT:
        return "ERR_NO_BITMAP_PLACEMENT";
    case PMM_INIT_ERR_ARITHMETIC_OVERFLOW:
        return "ERR_ARITHMETIC_OVERFLOW";
    case PMM_INIT_STATUS_UNKNOWN:
    default:
        return "UNKNOWN";
    }
}

const char *pmm_coverage_string(enum pmm_coverage coverage) {
    switch (coverage) {
    case PMM_COVERAGE_FULL:
        return "FULL";
    case PMM_COVERAGE_LIMITED:
        return "LIMITED";
    case PMM_COVERAGE_NONE:
    default:
        return "NONE";
    }
}

static size_t append_text(char *buffer, size_t size, size_t used,
                          const char *text) {
    if (text == NULL) {
        return used;
    }
    while (*text != '\0') {
        if (used + 1U < size) {
            buffer[used] = *text;
        }
        ++used;
        ++text;
    }
    if (size > 0U) {
        buffer[used < size ? used : size - 1U] = '\0';
    }
    return used;
}

static size_t append_u64(char *buffer, size_t size, size_t used,
                         uint64_t value) {
    char digits[20];
    size_t count = 0U;
    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value > 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        --count;
        if (used + 1U < size) {
            buffer[used] = digits[count];
        }
        ++used;
    }
    if (size > 0U) {
        buffer[used < size ? used : size - 1U] = '\0';
    }
    return used;
}

/* ITEM-393: bounded 0x-prefixed hexadecimal rendering, used by the records a
 * photograph has to carry. Always sixteen digits, so an address is readable
 * without counting zeros. */
static size_t append_hex64(char *buffer, size_t size, size_t used,
                           uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    used = append_text(buffer, size, used, "0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (used + 1U < size) {
            buffer[used] = digits[(value >> (unsigned int)shift) & 0xFU];
        }
        ++used;
    }
    if (size > 0U) {
        buffer[used < size ? used : size - 1U] = '\0';
    }
    return used;
}

size_t pmm_init_diagnostic_render(
    const struct pmm_init_diagnostic *report,
    char *buffer,
    size_t size)
{
    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (report == NULL) {
        return 0U;
    }
    size_t used = 0U;
    used = append_text(buffer, size, used, "status=");
    used = append_text(buffer, size, used,
                       pmm_init_status_string(report->status));
    used = append_text(buffer, size, used, " coverage=");
    used = append_text(buffer, size, used,
                       pmm_coverage_string(report->coverage));
    used = append_text(buffer, size, used, " pages=");
    used = append_u64(buffer, size, used, report->total_pages);
    used = append_text(buffer, size, used, " cap=");
    used = append_u64(buffer, size, used, report->page_cap);
    used = append_text(buffer, size, used, " usable=");
    used = append_u64(buffer, size, used, report->usable_bytes);
    used = append_text(buffer, size, used, " tracked=");
    used = append_u64(buffer, size, used, report->tracked_bytes);
    used = append_text(buffer, size, used, " dropped_pages=");
    used = append_u64(buffer, size, used, report->dropped_pages);
    used = append_text(buffer, size, used, " dropped_regions=");
    used = append_u64(buffer, size, used, report->dropped_regions);
    used = append_text(buffer, size, used, " regions=");
    used = append_u64(buffer, size, used, report->region_count);
    used = append_text(buffer, size, used, " bitmap_pages=");
    used = append_u64(buffer, size, used, report->bitmap_pages);
    used = append_text(buffer, size, used, " largest_region_pages=");
    used = append_u64(buffer, size, used, report->largest_region_pages);
    used = append_text(buffer, size, used, " placement_region=");
    used = append_u64(buffer, size, used, report->placement_region);
    used = append_text(buffer, size, used, " bitmap_phys=");
    used = append_u64(buffer, size, used, report->bitmap_phys);
    used = append_text(buffer, size, used, " first_region_base=");
    used = append_u64(buffer, size, used, report->first_region_base);
    used = append_text(buffer, size, used, " first_region_pages=");
    used = append_u64(buffer, size, used, report->first_region_pages);
    used = append_text(buffer, size, used, " largest_region_base=");
    used = append_u64(buffer, size, used, report->largest_region_base);
    used = append_text(buffer, size, used, " last_region_base=");
    used = append_u64(buffer, size, used, report->last_region_base);
    used = append_text(buffer, size, used, " bitmap_end_phys=");
    used = append_u64(buffer, size, used, report->bitmap_end_phys);
    return used;
}

const char *pmm_physical_class_string(enum pmm_physical_class value) {
    switch (value) {
    case PMM_PHYSICAL_UNALIGNED:
        return "UNALIGNED";
    case PMM_PHYSICAL_FREE:
        return "FREE";
    case PMM_PHYSICAL_ALLOCATED:
        return "ALLOCATED";
    case PMM_PHYSICAL_BITMAP:
        return "BITMAP";
    case PMM_PHYSICAL_UNTRACKED:
    default:
        return "UNTRACKED";
    }
}

enum pmm_physical_class pmm_classify_physical(uint64_t physical_address) {
    if (!pmm_ready || page_bitmap == NULL) {
        return PMM_PHYSICAL_UNTRACKED;
    }
    if (bitmap_page_count > 0U) {
        const uint64_t bitmap_end =
            bitmap_phys + (uint64_t)bitmap_page_count * PAGE_SIZE;
        if (physical_address >= bitmap_phys && physical_address < bitmap_end) {
            return PMM_PHYSICAL_BITMAP;
        }
    }
    if ((physical_address & (PAGE_SIZE - 1U)) != 0U) {
        for (uint32_t index = 0U; index < region_count; ++index) {
            if (physical_address >= regions[index].base &&
                physical_address < regions[index].end) {
                return PMM_PHYSICAL_UNALIGNED;
            }
        }
        return PMM_PHYSICAL_UNTRACKED;
    }
    uint64_t page_index = 0U;
    if (!phys_to_index(physical_address, &page_index)) {
        return PMM_PHYSICAL_UNTRACKED;
    }
    return bit_allocated(page_index) ? PMM_PHYSICAL_ALLOCATED
                                     : PMM_PHYSICAL_FREE;
}

size_t pmm_fault_line(
    uint64_t physical_address,
    char *buffer,
    size_t size)
{
    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';

    size_t used = 0U;
    used = append_text(buffer, size, used, "phys=");
    used = append_hex64(buffer, size, used, physical_address);
    used = append_text(buffer, size, used, " class=");
    used = append_text(buffer, size, used,
                       pmm_physical_class_string(
                           pmm_classify_physical(physical_address)));
    used = append_text(buffer, size, used, " region=");
    bool contained = false;
    for (uint32_t index = 0U; index < region_count; ++index) {
        if (physical_address >= regions[index].base &&
            physical_address < regions[index].end) {
            used = append_u64(buffer, size, used, (uint64_t)index);
            contained = true;
            break;
        }
    }
    if (!contained) {
        used = append_text(buffer, size, used, "none");
    }
    used = append_text(buffer, size, used, " regions=");
    used = append_u64(buffer, size, used, (uint64_t)region_count);
    used = append_text(buffer, size, used, " first_region_base=");
    used = append_hex64(buffer, size, used, diagnostic.first_region_base);
    used = append_text(buffer, size, used, " bitmap_phys=");
    used = append_hex64(buffer, size, used, diagnostic.bitmap_phys);
    used = append_text(buffer, size, used, " bitmap_end=");
    used = append_hex64(buffer, size, used, diagnostic.bitmap_end_phys);
    return used;
}

uint64_t pmm_total_usable_bytes(void) {
    return total_usable;
}

uint64_t pmm_free_page_count(void) {
    return pmm_ready ? free_page_count_value : 0U;
}

uint64_t pmm_alloc_page(void) {
    if (!pmm_ready || page_bitmap == NULL) {
        return 0U;
    }

    for (uint64_t word_index = 0U; word_index < bitmap_word_count;
         ++word_index) {
        const uint64_t word = page_bitmap[word_index];
        if (word == UINT64_MAX) {
            continue;
        }
        const uint64_t free_bits = ~word;
        const uint64_t bit = (uint64_t)__builtin_ctzll(free_bits);
        const uint64_t index = word_index * 64U + bit;
        if (index >= total_page_count) {
            continue;
        }
        const uint64_t physical = index_to_phys(index);
        if (physical == 0U) {
            continue;
        }
        set_bit_allocated(index);
        --free_page_count_value;
        zero_page(physical);
        return physical;
    }
    return 0U;
}

bool pmm_free_page(uint64_t physical_address) {
    return pmm_free_contiguous_pages(physical_address, 1U);
}

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    if (!pmm_ready || page_bitmap == NULL || count == 0U ||
        (uint64_t)count > free_page_count_value) {
        return 0U;
    }

    /*
     * ITEM-393: a contiguous run must lie inside one usable region.
     *
     * Bitmap indices are contiguous across region boundaries, so an index-only
     * first-fit scan can answer with a run whose first pages come from one
     * region and whose remaining pages come from the next one. Such a run is
     * not contiguous in the only sense that matters here: the caller receives
     * `physical` and treats [physical, physical + count * PAGE_SIZE) as one
     * buffer, while the allocator zeroes exactly that physical span - walking
     * out of the region, through whatever firmware hole or MMIO window follows
     * it, for a page count the map never described as RAM. The scan below
     * therefore stays inside a single region, which makes both the run and the
     * zeroing span a subset of that region by construction.
     */
    for (uint32_t region_index = 0U; region_index < region_count;
         ++region_index) {
        const struct pmm_region *region = &regions[region_index];
        if (region->page_count < (uint64_t)count) {
            continue;
        }
        for (uint64_t offset = 0U;
             offset + (uint64_t)count <= region->page_count; ++offset) {
            const uint64_t start = region->start_page + offset;
            bool available = true;
            for (uint32_t index = 0U; index < count; ++index) {
                if (bit_allocated(start + index)) {
                    available = false;
                    break;
                }
            }
            if (!available) {
                continue;
            }

            for (uint32_t index = 0U; index < count; ++index) {
                set_bit_allocated(start + index);
            }
            free_page_count_value -= count;
            const uint64_t physical = region->base + offset * PAGE_SIZE;
            for (uint32_t index = 0U; index < count; ++index) {
                zero_page(physical + (uint64_t)index * PAGE_SIZE);
            }
            return physical;
        }
    }
    return 0U;
}

bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count) {
    if (!pmm_ready || page_bitmap == NULL || count == 0U ||
        (physical_address & (PAGE_SIZE - 1U)) != 0U ||
        physical_address == 0U) {
        return false;
    }
    if (physical_address > UINT64_MAX -
                           (uint64_t)count * PAGE_SIZE) {
        return false;
    }

    const uint64_t range_end =
        physical_address + (uint64_t)count * PAGE_SIZE;
    if (range_touches_bitmap(physical_address, range_end)) {
        return false;
    }

    for (uint32_t offset = 0U; offset < count; ++offset) {
        uint64_t index = 0U;
        if (!phys_to_index(physical_address + (uint64_t)offset * PAGE_SIZE,
                           &index) ||
            !bit_allocated(index)) {
            return false;
        }
    }

    for (uint32_t offset = 0U; offset < count; ++offset) {
        uint64_t index = 0U;
        (void)phys_to_index(
            physical_address + (uint64_t)offset * PAGE_SIZE, &index);
        clear_bit_allocated(index);
        ++free_page_count_value;
    }
    return true;
}
