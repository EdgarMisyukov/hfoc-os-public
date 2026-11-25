/*
 * ITEM-625: total validation and bounded mapping of a static ELF64 image.
 */
#include "elf_user.h"

#include "pmm.h"

#include <string.h>

#define ELF_HEADER_SIZE 64U
#define ELF_PROGRAM_SIZE 56U
#define ELF_PAGE_SIZE 4096U
#define ELF_PT_LOAD 1U
#define ELF_PT_DYNAMIC 2U
#define ELF_PT_INTERP 3U
#define ELF_ET_EXEC 2U
#define ELF_EM_X86_64 62U
#define ELF_PF_X 0x1U
#define ELF_PF_W 0x2U
#define ELF_PF_R 0x4U
#define ELF_MAX_PAGES (ELF_USER_TOTAL_MAX / ELF_PAGE_SIZE)

static uint64_t mapped_virtual[ELF_MAX_PAGES];
static uint64_t mapped_physical[ELF_MAX_PAGES];

static uint16_t read_u16(const uint8_t *bytes)
{
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_u64(const uint8_t *bytes)
{
    return (uint64_t)read_u32(bytes) |
           ((uint64_t)read_u32(bytes + 4U) << 32);
}

static uint64_t elf_user_pages(uint64_t bytes)
{
    return (bytes + ELF_PAGE_SIZE - 1U) / ELF_PAGE_SIZE;
}

static bool ranges_overlap(uint64_t a_start, uint64_t a_size, uint64_t b_start,
                           uint64_t b_size)
{
    const uint64_t a_end = a_start + a_size;
    const uint64_t b_end = b_start + b_size;

    return a_start < b_end && b_start < a_end;
}

enum elf_user_status elf_user_parse(const void *bytes, size_t size,
                                    struct elf_user_image *image_out)
{
    const uint8_t *image = (const uint8_t *)bytes;
    uint64_t phoff;
    uint32_t phnum;
    uint32_t index;
    uint32_t count = 0U;
    uint32_t total_pages = 0U;

    if (bytes == NULL || image_out == NULL) {
        return ELF_USER_NULL;
    }
    memset(image_out, 0, sizeof(*image_out));
    if (size < ELF_HEADER_SIZE) {
        return ELF_USER_TOO_SMALL;
    }
    if (image[0] != 0x7FU || image[1] != 'E' || image[2] != 'L' ||
        image[3] != 'F') {
        return ELF_USER_BAD_MAGIC;
    }
    if (image[4] != 2U) {
        return ELF_USER_BAD_CLASS;
    }
    if (image[5] != 1U) {
        return ELF_USER_BAD_DATA;
    }
    if (image[6] != 1U) {
        return ELF_USER_BAD_VERSION;
    }
    if (read_u16(image + 16U) != ELF_ET_EXEC) {
        return ELF_USER_BAD_TYPE;
    }
    if (read_u16(image + 18U) != ELF_EM_X86_64) {
        return ELF_USER_BAD_MACHINE;
    }
    if (read_u32(image + 20U) != 1U) {
        return ELF_USER_BAD_VERSION;
    }
    if (read_u16(image + 52U) != ELF_HEADER_SIZE ||
        read_u16(image + 54U) != ELF_PROGRAM_SIZE) {
        return ELF_USER_BAD_HEADER;
    }
    phoff = read_u64(image + 32U);
    phnum = read_u16(image + 56U);
    if (phnum == 0U || phnum > ELF_USER_MAX_SEGMENTS) {
        return phnum == 0U ? ELF_USER_NO_SEGMENTS : ELF_USER_BAD_PROGRAM;
    }
    if (phoff > size ||
        (uint64_t)phnum * ELF_PROGRAM_SIZE > size - phoff) {
        return ELF_USER_BAD_PROGRAM;
    }
    image_out->entry = read_u64(image + 24U);
    for (index = 0U; index < phnum; ++index) {
        const uint8_t *program = image + phoff + index * ELF_PROGRAM_SIZE;
        const uint32_t type = read_u32(program);
        const uint32_t flags = read_u32(program + 4U);
        const uint64_t offset = read_u64(program + 8U);
        const uint64_t vaddr = read_u64(program + 16U);
        const uint64_t filesz = read_u64(program + 32U);
        const uint64_t memsz = read_u64(program + 40U);
        const uint64_t align = read_u64(program + 48U);
        struct elf_user_segment *segment;
        uint64_t pages;
        uint32_t earlier;

        if (type == ELF_PT_INTERP || type == ELF_PT_DYNAMIC) {
            return ELF_USER_BAD_PROGRAM;
        }
        if (type != ELF_PT_LOAD) {
            continue;
        }
        if ((flags & ~(uint32_t)(ELF_PF_R | ELF_PF_W | ELF_PF_X)) != 0U ||
            (flags & (ELF_PF_W | ELF_PF_X)) ==
                (ELF_PF_W | ELF_PF_X)) {
            return ELF_USER_BAD_PERMISSION;
        }
        if (memsz == 0U || filesz > memsz || filesz > size ||
            offset > size || filesz > size - offset) {
            return ELF_USER_BAD_RANGE;
        }
        if (align > 1U && (align & (align - 1U)) != 0U) {
            return ELF_USER_BAD_ALIGNMENT;
        }
        if ((vaddr & (ELF_PAGE_SIZE - 1U)) != 0U ||
            (offset & (ELF_PAGE_SIZE - 1U)) != 0U) {
            return ELF_USER_BAD_ALIGNMENT;
        }
        if (vaddr < VMM_USER_IMAGE_BASE ||
            vaddr > ELF_USER_IMAGE_LIMIT ||
            memsz > ELF_USER_SEGMENT_MAX ||
            memsz > ELF_USER_IMAGE_LIMIT - vaddr) {
            return ELF_USER_BAD_RANGE;
        }
        pages = elf_user_pages(memsz);
        if (pages > ELF_MAX_PAGES ||
            pages > ELF_MAX_PAGES - total_pages) {
            return ELF_USER_TOO_LARGE;
        }
        for (earlier = 0U; earlier < count; ++earlier) {
            if (ranges_overlap(vaddr, memsz,
                               image_out->segments[earlier].virtual_address,
                               image_out->segments[earlier].memory_size)) {
                return ELF_USER_BAD_OVERLAP;
            }
        }
        segment = &image_out->segments[count++];
        segment->virtual_address = vaddr;
        segment->memory_size = memsz;
        segment->file_size = filesz;
        segment->file_offset = offset;
        segment->flags = (flags & ELF_PF_R ? ELF_USER_FLAG_READ : 0U) |
                         (flags & ELF_PF_W ? ELF_USER_FLAG_WRITE : 0U) |
                         (flags & ELF_PF_X ? ELF_USER_FLAG_EXEC : 0U);
        total_pages += (uint32_t)pages;
    }
    if (count == 0U) {
        return ELF_USER_NO_SEGMENTS;
    }
    image_out->segment_count = count;
    image_out->total_pages = total_pages;
    {
        bool entry_executable = false;

        for (index = 0U; index < count; ++index) {
            const struct elf_user_segment *segment =
                &image_out->segments[index];

            if ((segment->flags & ELF_USER_FLAG_EXEC) != 0U &&
                image_out->entry >= segment->virtual_address &&
                image_out->entry < segment->virtual_address +
                                    segment->memory_size) {
                entry_executable = true;
            }
        }
        if (!entry_executable) {
            return ELF_USER_BAD_ENTRY;
        }
    }
    return ELF_USER_OK;
}

static void elf_user_zero_page(uint64_t physical, uint64_t hhdm_offset)
{
    uint8_t *page = (uint8_t *)(uintptr_t)(physical + hhdm_offset);
    size_t index;

    for (index = 0U; index < ELF_PAGE_SIZE; ++index) {
        page[index] = 0U;
    }
}

enum elf_user_status elf_user_map_tracked(
    const struct elf_user_image *image, const void *bytes, size_t size,
    struct address_space *address_space, uint64_t hhdm_offset,
    uint64_t *virtual_pages, uint64_t *physical_pages, uint32_t capacity,
    uint32_t *count_out)
{
    const uint8_t *file = (const uint8_t *)bytes;
    uint32_t segment_index;
    uint32_t mapped = 0U;

    if (count_out != NULL) {
        *count_out = 0U;
    }
    if (image == NULL || bytes == NULL || address_space == NULL) {
        return ELF_USER_NULL;
    }
    if ((virtual_pages == NULL) != (physical_pages == NULL) ||
        (virtual_pages != NULL && capacity == 0U)) {
        return ELF_USER_NULL;
    }
    if (image->segment_count == 0U ||
        image->segment_count > ELF_USER_MAX_SEGMENTS ||
        image->total_pages > ELF_MAX_PAGES) {
        return ELF_USER_BAD_PROGRAM;
    }
    for (segment_index = 0U; segment_index < image->segment_count;
         ++segment_index) {
        const struct elf_user_segment *segment =
            &image->segments[segment_index];
        const uint64_t pages = elf_user_pages(segment->memory_size);
        uint64_t page_index;

        if (segment->file_size > size ||
            segment->file_offset > size ||
            segment->file_size > size - segment->file_offset ||
            mapped > ELF_MAX_PAGES ||
            pages > ELF_MAX_PAGES - mapped) {
            goto rollback_bad;
        }
        for (page_index = 0U; page_index < pages; ++page_index) {
            const uint64_t virtual_address =
                segment->virtual_address + page_index * ELF_PAGE_SIZE;
            const uint64_t physical = pmm_alloc_page();
            uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;

            if (physical == 0U) {
                goto rollback;
            }
            elf_user_zero_page(physical, hhdm_offset);
            if (segment->file_size > page_index * ELF_PAGE_SIZE) {
                const uint64_t within = page_index * ELF_PAGE_SIZE;
                uint64_t take = segment->file_size - within;

                if (take > ELF_PAGE_SIZE) {
                    take = ELF_PAGE_SIZE;
                }
                memcpy((uint8_t *)(uintptr_t)(physical + hhdm_offset),
                       file + segment->file_offset + within, (size_t)take);
            }
            if ((segment->flags & ELF_USER_FLAG_WRITE) != 0U) {
                flags |= VMM_FLAG_WRITABLE;
            }
            if ((segment->flags & ELF_USER_FLAG_EXEC) == 0U) {
                flags |= VMM_FLAG_NX;
            }
            if (vmm_map_page_in(address_space, virtual_address, physical,
                                flags, hhdm_offset) != VMM_OK) {
                (void)pmm_free_page(physical);
                goto rollback;
            }
            if (virtual_pages != NULL) {
                if (mapped >= capacity) {
                    (void)vmm_unmap_page_in(address_space, virtual_address,
                                            hhdm_offset);
                    (void)pmm_free_page(physical);
                    goto rollback;
                }
                virtual_pages[mapped] = virtual_address;
                physical_pages[mapped] = physical;
            } else {
                mapped_virtual[mapped] = virtual_address;
                mapped_physical[mapped] = physical;
            }
            ++mapped;
        }
    }
    if (count_out != NULL) {
        *count_out = mapped;
    }
    return ELF_USER_OK;

rollback:
    if (virtual_pages == NULL) {
        while (mapped != 0U) {
            --mapped;
            (void)vmm_unmap_page_in(address_space, mapped_virtual[mapped],
                                    hhdm_offset);
            (void)pmm_free_page(mapped_physical[mapped]);
        }
    } else if (count_out != NULL) {
        *count_out = mapped;
    }
    return ELF_USER_IO;
rollback_bad:
    if (virtual_pages == NULL) {
        while (mapped != 0U) {
            --mapped;
            (void)vmm_unmap_page_in(address_space, mapped_virtual[mapped],
                                    hhdm_offset);
            (void)pmm_free_page(mapped_physical[mapped]);
        }
    } else if (count_out != NULL) {
        *count_out = mapped;
    }
    return ELF_USER_BAD_RANGE;
}

enum elf_user_status elf_user_map(const struct elf_user_image *image,
                                  const void *bytes, size_t size,
                                  struct address_space *address_space,
                                  uint64_t hhdm_offset)
{
    return elf_user_map_tracked(image, bytes, size, address_space, hhdm_offset,
                                NULL, NULL, 0U, NULL);
}

const char *elf_user_status_string(enum elf_user_status status)
{
    switch (status) {
    case ELF_USER_OK: return "ELF_USER_OK";
    case ELF_USER_NULL: return "ELF_USER_NULL";
    case ELF_USER_TOO_SMALL: return "ELF_USER_TOO_SMALL";
    case ELF_USER_BAD_MAGIC: return "ELF_USER_BAD_MAGIC";
    case ELF_USER_BAD_CLASS: return "ELF_USER_BAD_CLASS";
    case ELF_USER_BAD_DATA: return "ELF_USER_BAD_DATA";
    case ELF_USER_BAD_VERSION: return "ELF_USER_BAD_VERSION";
    case ELF_USER_BAD_TYPE: return "ELF_USER_BAD_TYPE";
    case ELF_USER_BAD_MACHINE: return "ELF_USER_BAD_MACHINE";
    case ELF_USER_BAD_HEADER: return "ELF_USER_BAD_HEADER";
    case ELF_USER_BAD_PROGRAM: return "ELF_USER_BAD_PROGRAM";
    case ELF_USER_BAD_RANGE: return "ELF_USER_BAD_RANGE";
    case ELF_USER_BAD_ALIGNMENT: return "ELF_USER_BAD_ALIGNMENT";
    case ELF_USER_BAD_OVERLAP: return "ELF_USER_BAD_OVERLAP";
    case ELF_USER_BAD_PERMISSION: return "ELF_USER_BAD_PERMISSION";
    case ELF_USER_BAD_ENTRY: return "ELF_USER_BAD_ENTRY";
    case ELF_USER_TOO_LARGE: return "ELF_USER_TOO_LARGE";
    case ELF_USER_NO_SEGMENTS: return "ELF_USER_NO_SEGMENTS";
    case ELF_USER_IO: return "ELF_USER_IO";
    default: return "ELF_USER_UNKNOWN";
    }
}
