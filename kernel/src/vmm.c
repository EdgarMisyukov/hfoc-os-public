#include "vmm.h"
#include "pmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PAGE_MASK                0x000FFFFFFFFFF000ULL
#define ENTRY_FLAGS_INTERMEDIATE 0x3ULL
#define ENTRY_FLAGS_MMIO_PTE     0x1BULL
#define VMM_FLAGS_MASK \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER | \
     VMM_FLAG_PWT | VMM_FLAG_PCD | VMM_FLAG_NX)
#define PS_BIT                   (1ULL << 7)

static bool vmm_initialized;
static uint64_t vmm_hhdm_offset_value;
static struct address_space kernel_address_space;
static struct address_space current_address_space;

#ifdef HFOC_VMM_HOST_TEST
extern void *vmm_test_phys_to_virt(uint64_t physical);
static void *vmm_phys_to_virt(uint64_t physical, uint64_t hhdm_offset) {
    (void)hhdm_offset;
    return vmm_test_phys_to_virt(physical);
}
extern uint64_t mock_read_cr3(void);
extern void mock_write_cr3(uint64_t cr3);
extern void mock_invlpg(uint64_t virtual_address);
static uint64_t read_cr3(void) {
    return mock_read_cr3();
}
static void write_cr3(uint64_t cr3) {
    mock_write_cr3(cr3);
}
static void invlpg(uint64_t virtual_address) {
    mock_invlpg(virtual_address);
}
#else
static void *vmm_phys_to_virt(uint64_t physical, uint64_t hhdm_offset) {
    return (void *)(uintptr_t)(physical + hhdm_offset);
}
static uint64_t read_cr3(void) {
    uint64_t cr3;
    __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    return cr3;
}
static void write_cr3(uint64_t cr3) {
    __asm__ volatile ("mov %0, %%cr3" : : "r"(cr3) : "memory");
}
static void invlpg(uint64_t virtual_address) {
    __asm__ volatile ("invlpg (%0)" : : "r"(virtual_address) : "memory");
}
#endif

#ifdef HFOC_VMM_HOST_TEST
extern void vmm_test_note_page_zeroed(uint64_t physical);
#endif

static void zero_page(uint64_t physical, uint64_t hhdm_offset) {
    unsigned char *page = (unsigned char *)vmm_phys_to_virt(
        physical, hhdm_offset);
    for (size_t index = 0U; index < 4096U; ++index) {
        page[index] = 0U;
    }
#ifdef HFOC_VMM_HOST_TEST
    vmm_test_note_page_zeroed(physical);
#endif
}

static bool address_space_valid(const struct address_space *as) {
    return as != NULL && as->pml4_phys != 0U &&
           (as->pml4_phys & 0xFFFULL) == 0U;
}

const char *vmm_status_string(enum vmm_status status) {
    switch (status) {
    case VMM_OK:                  return "OK";
    case VMM_BAD_ALIGNMENT:       return "bad alignment";
    case VMM_HHDM_OVERFLOW:       return "HHDM overflow";
    case VMM_PMM_ALLOC_FAILED:    return "PMM allocation failed";
    case VMM_LARGE_PAGE_CONFLICT: return "large page conflict";
    case VMM_PTE_CONFLICT:        return "PTE conflict";
    case VMM_BAD_OUTPUT_POINTER:  return "bad output pointer";
    case VMM_BAD_ADDRESS_SPACE:   return "bad address space";
    case VMM_NOT_MAPPED:          return "not mapped";
    case VMM_BAD_RANGE:           return "bad range";
    case VMM_BAD_FLAGS:           return "bad flags";
    default:                      return "unknown";
    }
}

enum vmm_status vmm_init(uint64_t hhdm_offset) {
    const uint64_t cr3 = read_cr3() & PAGE_MASK;
    if (cr3 == 0U) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    vmm_hhdm_offset_value = hhdm_offset;
    kernel_address_space.pml4_phys = cr3;
    current_address_space = kernel_address_space;
    vmm_initialized = true;
    return VMM_OK;
}

enum vmm_status vmm_create_address_space(
    struct address_space *out,
    uint64_t hhdm_offset)
{
    if (out == NULL) {
        return VMM_BAD_OUTPUT_POINTER;
    }
    if (!vmm_initialized) {
        return VMM_BAD_ADDRESS_SPACE;
    }

    const uint64_t pml4_phys = pmm_alloc_page();
    if (pml4_phys == 0U) {
        return VMM_PMM_ALLOC_FAILED;
    }
    zero_page(pml4_phys, hhdm_offset);

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(
        pml4_phys, hhdm_offset);
    const uint64_t *kernel_pml4 = (const uint64_t *)vmm_phys_to_virt(
        kernel_address_space.pml4_phys, hhdm_offset);
    for (uint64_t index = 256U; index < 512U; ++index) {
        pml4[index] = kernel_pml4[index];
    }

    out->pml4_phys = pml4_phys;
    return VMM_OK;
}

enum vmm_status vmm_destroy_address_space(
    struct address_space *as,
    uint64_t hhdm_offset)
{
    if (as == NULL || !vmm_initialized ||
        !address_space_valid(as)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    if (as->pml4_phys == kernel_address_space.pml4_phys ||
        as->pml4_phys == current_address_space.pml4_phys) {
        return VMM_BAD_ADDRESS_SPACE;
    }

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(
        as->pml4_phys, hhdm_offset);
    for (uint64_t pml4_idx = 0U; pml4_idx < 256U; ++pml4_idx) {
        const uint64_t pml4e = pml4[pml4_idx];
        if ((pml4e & 1U) == 0U) {
            continue;
        }
        uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(
            pml4e & PAGE_MASK, hhdm_offset);
        for (uint64_t pdpt_idx = 0U; pdpt_idx < 512U; ++pdpt_idx) {
            const uint64_t pdpte = pdpt[pdpt_idx];
            if ((pdpte & 1U) == 0U) {
                continue;
            }
            if ((pdpte & PS_BIT) != 0U) {
                continue;
            }
            uint64_t *pd = (uint64_t *)vmm_phys_to_virt(
                pdpte & PAGE_MASK, hhdm_offset);
            for (uint64_t pd_idx = 0U; pd_idx < 512U; ++pd_idx) {
                const uint64_t pde = pd[pd_idx];
                if ((pde & 1U) == 0U) {
                    continue;
                }
                if ((pde & PS_BIT) != 0U) {
                    continue;
                }
                uint64_t *pt = (uint64_t *)vmm_phys_to_virt(
                    pde & PAGE_MASK, hhdm_offset);
                for (uint64_t pt_idx = 0U; pt_idx < 512U; ++pt_idx) {
                    if ((pt[pt_idx] & 1U) != 0U) {
                        pt[pt_idx] = 0U;
                    }
                }
                if (!pmm_free_page(pde & PAGE_MASK)) {
                    return VMM_BAD_ADDRESS_SPACE;
                }
            }
            if (!pmm_free_page(pdpte & PAGE_MASK)) {
                return VMM_BAD_ADDRESS_SPACE;
            }
        }
        if (!pmm_free_page(pml4e & PAGE_MASK)) {
            return VMM_BAD_ADDRESS_SPACE;
        }
        pml4[pml4_idx] = 0U;
    }

    if (!pmm_free_page(as->pml4_phys)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    as->pml4_phys = 0U;
    return VMM_OK;
}

enum vmm_status vmm_map_page_in(
    const struct address_space *as,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags,
    uint64_t hhdm_offset)
{
    if (!address_space_valid(as)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    if ((virtual_address & 0xFFFULL) != 0U ||
        (physical_address & 0xFFFULL) != 0U) {
        return VMM_BAD_ALIGNMENT;
    }

    const uint64_t pml4_idx = (virtual_address >> 39) & 0x1FFU;
    const uint64_t pdpt_idx = (virtual_address >> 30) & 0x1FFU;
    const uint64_t pd_idx   = (virtual_address >> 21) & 0x1FFU;
    const uint64_t pt_idx   = (virtual_address >> 12) & 0x1FFU;

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(
        as->pml4_phys, hhdm_offset);
    const uint64_t pml4e = pml4[pml4_idx];

    bool need_pdpt = !(pml4e & 1U);
    uint64_t *pdpt = NULL;
    uint64_t pdpte = 0U;
    if (!need_pdpt) {
        pdpt = (uint64_t *)vmm_phys_to_virt(
            pml4e & PAGE_MASK, hhdm_offset);
        pdpte = pdpt[pdpt_idx];
        if ((pdpte & 1U) != 0U && (pdpte & PS_BIT) != 0U) {
            return VMM_LARGE_PAGE_CONFLICT;
        }
    }

    bool need_pd = !(pdpte & 1U);
    uint64_t *pd = NULL;
    uint64_t pde = 0U;
    if (!need_pd) {
        pd = (uint64_t *)vmm_phys_to_virt(
            pdpte & PAGE_MASK, hhdm_offset);
        pde = pd[pd_idx];
        if ((pde & 1U) != 0U && (pde & PS_BIT) != 0U) {
            return VMM_LARGE_PAGE_CONFLICT;
        }
    }

    bool need_pt = !(pde & 1U);
    uint64_t *pt = NULL;
    uint64_t pte = 0U;
    if (!need_pt) {
        pt = (uint64_t *)vmm_phys_to_virt(
            pde & PAGE_MASK, hhdm_offset);
        pte = pt[pt_idx];
    }

    if ((pte & 1U) != 0U) {
        if ((pte & PAGE_MASK) == physical_address &&
            (pte & VMM_FLAGS_MASK) == flags) {
            return VMM_OK;
        }
        return VMM_PTE_CONFLICT;
    }

    uint64_t pdpt_phys = 0U;
    uint64_t pd_phys = 0U;
    uint64_t pt_phys = 0U;

    if (need_pdpt) {
        pdpt_phys = pmm_alloc_page();
        if (pdpt_phys == 0U) {
            return VMM_PMM_ALLOC_FAILED;
        }
        zero_page(pdpt_phys, hhdm_offset);
        pdpt = (uint64_t *)vmm_phys_to_virt(
            pdpt_phys, hhdm_offset);
    }
    if (need_pd) {
        pd_phys = pmm_alloc_page();
        if (pd_phys == 0U) {
            if (pdpt_phys != 0U) {
                (void)pmm_free_page(pdpt_phys);
            }
            return VMM_PMM_ALLOC_FAILED;
        }
        zero_page(pd_phys, hhdm_offset);
        pd = (uint64_t *)vmm_phys_to_virt(pd_phys, hhdm_offset);
    }
    if (need_pt) {
        pt_phys = pmm_alloc_page();
        if (pt_phys == 0U) {
            if (pdpt_phys != 0U) {
                (void)pmm_free_page(pdpt_phys);
            }
            if (pd_phys != 0U) {
                (void)pmm_free_page(pd_phys);
            }
            return VMM_PMM_ALLOC_FAILED;
        }
        zero_page(pt_phys, hhdm_offset);
        pt = (uint64_t *)vmm_phys_to_virt(pt_phys, hhdm_offset);
    }

    const uint64_t intermediate_flags =
        ENTRY_FLAGS_INTERMEDIATE | (flags & VMM_FLAG_USER);
    if (need_pdpt) {
        pml4[pml4_idx] = pdpt_phys | intermediate_flags;
    }
    if (need_pd) {
        pdpt[pdpt_idx] = pd_phys | intermediate_flags;
    }
    if (need_pt) {
        pd[pd_idx] = pt_phys | intermediate_flags;
    }
    pt[pt_idx] = physical_address | flags;

    if (as->pml4_phys == current_address_space.pml4_phys) {
        invlpg(virtual_address);
    }
    return VMM_OK;
}

enum vmm_status vmm_unmap_page_in(
    const struct address_space *as,
    uint64_t virtual_address,
    uint64_t hhdm_offset)
{
    if (!address_space_valid(as)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    if ((virtual_address & 0xFFFULL) != 0U) {
        return VMM_BAD_ALIGNMENT;
    }

    const uint64_t pml4_idx = (virtual_address >> 39) & 0x1FFU;
    const uint64_t pdpt_idx = (virtual_address >> 30) & 0x1FFU;
    const uint64_t pd_idx   = (virtual_address >> 21) & 0x1FFU;
    const uint64_t pt_idx   = (virtual_address >> 12) & 0x1FFU;

    uint64_t *pml4 = (uint64_t *)vmm_phys_to_virt(
        as->pml4_phys, hhdm_offset);
    const uint64_t pml4e = pml4[pml4_idx];
    if ((pml4e & 1U) == 0U) {
        return VMM_NOT_MAPPED;
    }

    uint64_t *pdpt = (uint64_t *)vmm_phys_to_virt(
        pml4e & PAGE_MASK, hhdm_offset);
    const uint64_t pdpte = pdpt[pdpt_idx];
    if ((pdpte & 1U) == 0U) {
        return VMM_NOT_MAPPED;
    }

    uint64_t *pd = (uint64_t *)vmm_phys_to_virt(
        pdpte & PAGE_MASK, hhdm_offset);
    const uint64_t pde = pd[pd_idx];
    if ((pde & 1U) == 0U) {
        return VMM_NOT_MAPPED;
    }

    uint64_t *pt = (uint64_t *)vmm_phys_to_virt(
        pde & PAGE_MASK, hhdm_offset);
    if ((pt[pt_idx] & 1U) == 0U) {
        return VMM_NOT_MAPPED;
    }

    pt[pt_idx] = 0U;
    if (as->pml4_phys == current_address_space.pml4_phys) {
        invlpg(virtual_address);
    }
    return VMM_OK;
}

enum vmm_status vmm_validate_user_range(
    uint64_t virtual_address,
    uint64_t page_count)
{
    if ((virtual_address & 0xFFFULL) != 0U) {
        return VMM_BAD_ALIGNMENT;
    }
    if (page_count == 0U || virtual_address < VMM_USER_IMAGE_BASE ||
        virtual_address >= VMM_USER_STACK_TOP) {
        return VMM_BAD_RANGE;
    }
    if (page_count > (VMM_USER_STACK_TOP - virtual_address) / 4096ULL) {
        return VMM_BAD_RANGE;
    }
    return VMM_OK;
}

static enum vmm_status vmm_validate_user_page(
    const struct address_space *as, uint64_t virtual_address, bool write)
{
    const uint64_t pml4_index = (virtual_address >> 39U) & 0x1FFU;
    const uint64_t pdpt_index = (virtual_address >> 30U) & 0x1FFU;
    const uint64_t pd_index = (virtual_address >> 21U) & 0x1FFU;
    const uint64_t pt_index = (virtual_address >> 12U) & 0x1FFU;
    const uint64_t *pml4 = (const uint64_t *)vmm_phys_to_virt(
        as->pml4_phys, vmm_hhdm_offset_value);
    const uint64_t pml4e = pml4[pml4_index];
    const uint64_t *pdpt;
    uint64_t pdpte;
    const uint64_t *pd;
    uint64_t pde;
    const uint64_t *pt;
    uint64_t pte;

    if ((pml4e & VMM_FLAG_PRESENT) == 0U) {
        return VMM_NOT_MAPPED;
    }
    if ((pml4e & VMM_FLAG_USER) == 0U) {
        return VMM_BAD_FLAGS;
    }
    pdpt = (const uint64_t *)vmm_phys_to_virt(pml4e & PAGE_MASK,
                                              vmm_hhdm_offset_value);
    pdpte = pdpt[pdpt_index];
    if ((pdpte & VMM_FLAG_PRESENT) == 0U) {
        return VMM_NOT_MAPPED;
    }
    if ((pdpte & VMM_FLAG_USER) == 0U || (pdpte & PS_BIT) != 0U) {
        return VMM_BAD_FLAGS;
    }
    pd = (const uint64_t *)vmm_phys_to_virt(pdpte & PAGE_MASK,
                                            vmm_hhdm_offset_value);
    pde = pd[pd_index];
    if ((pde & VMM_FLAG_PRESENT) == 0U) {
        return VMM_NOT_MAPPED;
    }
    if ((pde & VMM_FLAG_USER) == 0U || (pde & PS_BIT) != 0U) {
        return VMM_BAD_FLAGS;
    }
    pt = (const uint64_t *)vmm_phys_to_virt(pde & PAGE_MASK,
                                            vmm_hhdm_offset_value);
    pte = pt[pt_index];
    if ((pte & VMM_FLAG_PRESENT) == 0U) {
        return VMM_NOT_MAPPED;
    }
    if ((pte & VMM_FLAG_USER) == 0U) {
        return VMM_BAD_FLAGS;
    }
    if (write && (pte & VMM_FLAG_WRITABLE) == 0U) {
        return VMM_BAD_FLAGS;
    }
    return VMM_OK;
}

enum vmm_status vmm_validate_user_buffer(
    const struct address_space *as,
    uint64_t address,
    uint64_t length,
    bool write)
{
    uint64_t page;
    uint64_t last;

    if (!address_space_valid(as)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    if (length == 0U || length > VMM_USER_BUFFER_MAX) {
        return VMM_BAD_RANGE;
    }
    if (address < VMM_USER_IMAGE_BASE || address >= VMM_USER_STACK_TOP ||
        length > VMM_USER_STACK_TOP - address) {
        return VMM_BAD_RANGE;
    }
    page = address & PAGE_MASK;
    last = (address + length - 1U) & PAGE_MASK;
    for (;;) {
        const enum vmm_status status =
            vmm_validate_user_page(as, page, write);

        if (status != VMM_OK) {
            return status;
        }
        if (page == last) {
            break;
        }
        page += 4096U;
    }
    return VMM_OK;
}

enum vmm_status vmm_map_user_pages_in(
    const struct address_space *as,
    uint64_t virtual_address,
    const uint64_t *physical_pages,
    uint64_t page_count,
    uint64_t user_flags,
    uint64_t hhdm_offset)
{
    uint64_t index;
    enum vmm_status status;

    if (!address_space_valid(as)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    if (physical_pages == NULL) {
        return VMM_BAD_OUTPUT_POINTER;
    }
    status = vmm_validate_user_range(virtual_address, page_count);
    if (status != VMM_OK) {
        return status;
    }
    if ((user_flags & VMM_FLAGS_MASK) != user_flags ||
        (user_flags & (VMM_FLAG_PRESENT | VMM_FLAG_USER)) !=
            (VMM_FLAG_PRESENT | VMM_FLAG_USER)) {
        return VMM_BAD_FLAGS;
    }
    for (index = 0U; index < page_count; ++index) {
        if ((physical_pages[index] & 0xFFFULL) != 0U) {
            return VMM_BAD_ALIGNMENT;
        }
    }

    for (index = 0U; index < page_count; ++index) {
        const uint64_t virtual_page = virtual_address + index * 4096ULL;
        status = vmm_map_page_in(as, virtual_page, physical_pages[index],
                                 user_flags, hhdm_offset);
        if (status != VMM_OK) {
            uint64_t rollback_index;

            for (rollback_index = 0U; rollback_index < index;
                 ++rollback_index) {
                (void)vmm_unmap_page_in(
                    as, virtual_address + rollback_index * 4096ULL,
                    hhdm_offset);
            }
            return status;
        }
    }
    return VMM_OK;
}

enum vmm_status vmm_activate_address_space(struct address_space *as) {
    if (!address_space_valid(as)) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    write_cr3(as->pml4_phys);
    current_address_space = *as;
    return VMM_OK;
}

const struct address_space *vmm_kernel_address_space(void) {
    return vmm_initialized ? &kernel_address_space : NULL;
}

const struct address_space *vmm_current_address_space(void) {
    return vmm_initialized ? &current_address_space : NULL;
}

enum vmm_status vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual)
{
    if (out_virtual == NULL) {
        return VMM_BAD_OUTPUT_POINTER;
    }
    if ((physical_address & 0xFFFULL) != 0U) {
        return VMM_BAD_ALIGNMENT;
    }
    if (physical_address > UINT64_MAX - hhdm_offset) {
        return VMM_HHDM_OVERFLOW;
    }
    if (!vmm_initialized) {
        const enum vmm_status init_status = vmm_init(hhdm_offset);
        if (init_status != VMM_OK) {
            return init_status;
        }
    }

    const uint64_t virtual_address = physical_address + hhdm_offset;
    const enum vmm_status status = vmm_map_page_in(
        &kernel_address_space,
        virtual_address,
        physical_address,
        ENTRY_FLAGS_MMIO_PTE,
        hhdm_offset);
    if (status == VMM_OK) {
        *out_virtual = (volatile void *)virtual_address;
    }
    return status;
}
