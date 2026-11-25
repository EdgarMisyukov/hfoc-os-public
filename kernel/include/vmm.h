#ifndef HFOC_VMM_H
#define HFOC_VMM_H

#include <stdbool.h>
#include <stdint.h>

struct address_space {
    uint64_t pml4_phys;
};

#define VMM_FLAG_PRESENT  0x1ULL
#define VMM_FLAG_WRITABLE 0x2ULL
#define VMM_FLAG_USER     0x4ULL
#define VMM_FLAG_PWT      0x8ULL
#define VMM_FLAG_PCD      0x10ULL
#define VMM_FLAG_NX       (1ULL << 63U)

#define VMM_USER_IMAGE_BASE  0x00400000ULL
#define VMM_USER_STACK_BASE  0x00007FFFF0000000ULL
#define VMM_USER_STACK_TOP   0x0000800000000000ULL

enum vmm_status {
    VMM_OK = 0,
    VMM_BAD_ALIGNMENT = 1,
    VMM_HHDM_OVERFLOW = 2,
    VMM_PMM_ALLOC_FAILED = 3,
    VMM_LARGE_PAGE_CONFLICT = 4,
    VMM_PTE_CONFLICT = 5,
    VMM_BAD_OUTPUT_POINTER = 6,
    VMM_BAD_ADDRESS_SPACE = 7,
    VMM_NOT_MAPPED = 8,
    VMM_BAD_RANGE = 9,
    VMM_BAD_FLAGS = 10,
};

enum vmm_status vmm_init(uint64_t hhdm_offset);
enum vmm_status vmm_create_address_space(
    struct address_space *out,
    uint64_t hhdm_offset);
enum vmm_status vmm_destroy_address_space(
    struct address_space *as,
    uint64_t hhdm_offset);
enum vmm_status vmm_map_page_in(
    const struct address_space *as,
    uint64_t virtual_address,
    uint64_t physical_address,
    uint64_t flags,
    uint64_t hhdm_offset);
enum vmm_status vmm_unmap_page_in(
    const struct address_space *as,
    uint64_t virtual_address,
    uint64_t hhdm_offset);
enum vmm_status vmm_validate_user_range(
    uint64_t virtual_address,
    uint64_t page_count);
/*
 * ITEM-624: validate a user byte buffer against the page tables before any
 * byte is copied.  Every page in [address, address+length) must be present,
 * user-accessible and, for a write, writable; huge pages are refused in the
 * user range and the whole buffer is bounded so a malformed length cannot
 * make the kernel walk an unbounded table.
 */
#define VMM_USER_BUFFER_MAX (1024U * 1024U)
enum vmm_status vmm_validate_user_buffer(
    const struct address_space *as,
    uint64_t address,
    uint64_t length,
    bool write);
enum vmm_status vmm_map_user_pages_in(
    const struct address_space *as,
    uint64_t virtual_address,
    const uint64_t *physical_pages,
    uint64_t page_count,
    uint64_t user_flags,
    uint64_t hhdm_offset);
enum vmm_status vmm_activate_address_space(
    struct address_space *as);
const struct address_space *vmm_kernel_address_space(void);
const struct address_space *vmm_current_address_space(void);

enum vmm_status vmm_map_mmio_page(
    uint64_t physical_address,
    uint64_t hhdm_offset,
    volatile void **out_virtual);

const char *vmm_status_string(enum vmm_status status);

#endif
