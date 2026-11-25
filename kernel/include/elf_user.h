#ifndef HFOC_ELF_USER_H
#define HFOC_ELF_USER_H

#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-625: the native static ELF64 user image.
 *
 * Only ET_EXEC, x86-64, little-endian, static images are accepted.  Dynamic
 * interpreters, dynamic sections, overlapping load segments, kernel/user
 * range escapes and W+X segments are refused explicitly.  Parsing is total
 * over the bounded input; mapping allocates and publishes nothing until the
 * whole validated image fits the caller's limits.
 */
#define ELF_USER_MAX_SEGMENTS 16U
#define ELF_USER_IMAGE_LIMIT \
    (VMM_USER_IMAGE_BASE + 64ULL * 1024ULL * 1024ULL)
#define ELF_USER_SEGMENT_MAX (8ULL * 1024ULL * 1024ULL)
#define ELF_USER_TOTAL_MAX (16ULL * 1024ULL * 1024ULL)

#define ELF_USER_FLAG_READ  0x1U
#define ELF_USER_FLAG_WRITE 0x2U
#define ELF_USER_FLAG_EXEC  0x4U

enum elf_user_status {
    ELF_USER_OK = 0,
    ELF_USER_NULL = 1,
    ELF_USER_TOO_SMALL = 2,
    ELF_USER_BAD_MAGIC = 3,
    ELF_USER_BAD_CLASS = 4,
    ELF_USER_BAD_DATA = 5,
    ELF_USER_BAD_VERSION = 6,
    ELF_USER_BAD_TYPE = 7,
    ELF_USER_BAD_MACHINE = 8,
    ELF_USER_BAD_HEADER = 9,
    ELF_USER_BAD_PROGRAM = 10,
    ELF_USER_BAD_RANGE = 11,
    ELF_USER_BAD_ALIGNMENT = 12,
    ELF_USER_BAD_OVERLAP = 13,
    ELF_USER_BAD_PERMISSION = 14,
    ELF_USER_BAD_ENTRY = 15,
    ELF_USER_TOO_LARGE = 16,
    ELF_USER_NO_SEGMENTS = 17,
    ELF_USER_IO = 18,
};

struct elf_user_segment {
    uint64_t virtual_address;
    uint64_t memory_size;
    uint64_t file_size;
    uint64_t file_offset;
    uint32_t flags;
};

struct elf_user_image {
    uint64_t entry;
    uint32_t segment_count;
    uint32_t total_pages;
    struct elf_user_segment segments[ELF_USER_MAX_SEGMENTS];
};

enum elf_user_status elf_user_parse(const void *bytes, size_t size,
                                    struct elf_user_image *image_out);
/*
 * Map the validated image into `address_space`.  Every page is allocated and
 * cleared before it is mapped; on any failure the already mapped pages are
 * unmapped and freed.  `bytes` is the same buffer that was parsed.
 */
enum elf_user_status elf_user_map(const struct elf_user_image *image,
                                  const void *bytes, size_t size,
                                  struct address_space *address_space,
                                  uint64_t hhdm_offset);
/* Same mapping, recording each mapped page for the process owner. */
enum elf_user_status elf_user_map_tracked(
    const struct elf_user_image *image, const void *bytes, size_t size,
    struct address_space *address_space, uint64_t hhdm_offset,
    uint64_t *virtual_pages, uint64_t *physical_pages, uint32_t capacity,
    uint32_t *count_out);
const char *elf_user_status_string(enum elf_user_status status);

#endif
