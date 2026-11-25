/*
 * ITEM-625 host proof for the static ELF64 image parser and mapper.
 *
 * The fixture is assembled byte by byte here, so the parser is checked
 * against the ELF64 layout and its refusals, not against a linker output the
 * test happens to accept.  Mapping itself is driven in the target/QEMU proof;
 * this host proof pins the total validation before any allocation.
 */
#include "elf_user.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ELF_SIZE 0x1020U
#define TEXT_OFFSET 0x1000U

static uint8_t fixture[ELF_SIZE];
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

#define MOCK_PAGES 8U

static uint8_t mock_memory[MOCK_PAGES * 4096U]
    __attribute__((aligned(4096)));
static bool mock_used[MOCK_PAGES];
static uint64_t mock_virtual[MOCK_PAGES];
static uint64_t mock_physical[MOCK_PAGES];
static uint32_t mock_map_count;
static uint32_t mock_free_count;
static uint32_t mock_alloc_fail_after = MOCK_PAGES;

uint64_t pmm_alloc_page(void) {
    uint32_t index;

    for (index = 0U; index < MOCK_PAGES; ++index) {
        if (!mock_used[index]) {
            if (index >= mock_alloc_fail_after) {
                return 0U;
            }
            mock_used[index] = true;
            return (uint64_t)(uintptr_t)(mock_memory + index * 4096U);
        }
    }
    return 0U;
}

bool pmm_free_page(uint64_t physical) {
    uint32_t index;

    for (index = 0U; index < MOCK_PAGES; ++index) {
        if (mock_used[index] &&
            (uint64_t)(uintptr_t)(mock_memory + index * 4096U) == physical) {
            mock_used[index] = false;
            ++mock_free_count;
            return true;
        }
    }
    return false;
}

enum vmm_status vmm_map_page_in(const struct address_space *as,
                                uint64_t virtual_address,
                                uint64_t physical_address, uint64_t flags,
                                uint64_t hhdm_offset)
{
    (void)as; (void)flags; (void)hhdm_offset;
    if (mock_map_count >= MOCK_PAGES) {
        return VMM_PTE_CONFLICT;
    }
    mock_virtual[mock_map_count] = virtual_address;
    mock_physical[mock_map_count] = physical_address;
    ++mock_map_count;
    return VMM_OK;
}

enum vmm_status vmm_unmap_page_in(const struct address_space *as,
                                  uint64_t virtual_address,
                                  uint64_t hhdm_offset)
{
    (void)as; (void)hhdm_offset;
    if (mock_map_count != 0U &&
        mock_virtual[mock_map_count - 1U] == virtual_address) {
        --mock_map_count;
        return VMM_OK;
    }
    return VMM_NOT_MAPPED;
}

static void reset_mocks(void) {
    memset(mock_used, 0, sizeof(mock_used));
    memset(mock_memory, 0, sizeof(mock_memory));
    mock_map_count = 0U;
    mock_free_count = 0U;
    mock_alloc_fail_after = MOCK_PAGES;
}

static void put_u16(size_t offset, uint16_t value)
{
    fixture[offset] = (uint8_t)value;
    fixture[offset + 1U] = (uint8_t)(value >> 8);
}

static void put_u32(size_t offset, uint32_t value)
{
    fixture[offset] = (uint8_t)value;
    fixture[offset + 1U] = (uint8_t)(value >> 8);
    fixture[offset + 2U] = (uint8_t)(value >> 16);
    fixture[offset + 3U] = (uint8_t)(value >> 24);
}

static void put_u64(size_t offset, uint64_t value)
{
    put_u32(offset, (uint32_t)value);
    put_u32(offset + 4U, (uint32_t)(value >> 32));
}

static void build_fixture(void)
{
    memset(fixture, 0, sizeof(fixture));
    fixture[0] = 0x7FU;
    fixture[1] = 'E';
    fixture[2] = 'L';
    fixture[3] = 'F';
    fixture[4] = 2U;
    fixture[5] = 1U;
    fixture[6] = 1U;
    put_u16(16U, 2U);
    put_u16(18U, 62U);
    put_u32(20U, 1U);
    put_u64(24U, 0x400000ULL);
    put_u64(32U, 64U);
    put_u16(52U, 64U);
    put_u16(54U, 56U);
    put_u16(56U, 1U);
    put_u16(58U, 56U);
    /* One PT_LOAD: RX, file and memory size 0x20. */
    put_u32(64U, 1U);
    put_u32(68U, 5U);
    put_u64(72U, TEXT_OFFSET);
    put_u64(80U, 0x400000ULL);
    put_u64(88U, 0x400000ULL);
    put_u64(96U, 0x20U);
    put_u64(104U, 0x20U);
    put_u64(112U, 0x1000U);
    fixture[TEXT_OFFSET] = 0x90U;
}

static void expect(enum elf_user_status expected, const char *message)
{
    struct elf_user_image image;
    const enum elf_user_status actual =
        elf_user_parse(fixture, sizeof(fixture), &image);

    CHECK(actual == expected, message);
}

static void test_valid(void)
{
    struct elf_user_image image;

    build_fixture();
    CHECK(elf_user_parse(fixture, sizeof(fixture), &image) == ELF_USER_OK,
          "a valid static ELF64 image parses");
    CHECK(image.entry == 0x400000ULL && image.segment_count == 1U &&
              image.total_pages == 1U,
          "the parsed entry and page count are exact");
    CHECK(image.segments[0].virtual_address == 0x400000ULL &&
              image.segments[0].file_size == 0x20U &&
              image.segments[0].flags ==
                  (ELF_USER_FLAG_READ | ELF_USER_FLAG_EXEC),
          "the load segment keeps its address, size and RX flags");
    CHECK(strcmp(elf_user_status_string(ELF_USER_OK), "ELF_USER_OK") == 0,
          "the status strings are stable");
}

static void test_refusals(void)
{
    build_fixture();
    fixture[0] = 0U;
    expect(ELF_USER_BAD_MAGIC, "a wrong magic is refused");

    build_fixture();
    fixture[4] = 1U;
    expect(ELF_USER_BAD_CLASS, "a 32-bit image is refused");

    build_fixture();
    put_u16(16U, 3U);
    expect(ELF_USER_BAD_TYPE, "a dynamic executable type is refused");

    build_fixture();
    put_u16(18U, 183U);
    expect(ELF_USER_BAD_MACHINE, "a foreign machine is refused");

    build_fixture();
    put_u16(54U, 32U);
    expect(ELF_USER_BAD_HEADER, "a wrong program-header size is refused");

    build_fixture();
    put_u32(64U, 3U);
    expect(ELF_USER_BAD_PROGRAM, "a PT_INTERP image is refused");

    build_fixture();
    put_u32(68U, 7U);
    expect(ELF_USER_BAD_PERMISSION, "a W+X segment is refused");

    build_fixture();
    put_u64(96U, 0x2000U);
    expect(ELF_USER_BAD_RANGE, "a file size above the memory size is refused");

    build_fixture();
    put_u64(80U, 0x400001ULL);
    expect(ELF_USER_BAD_ALIGNMENT, "an unaligned load address is refused");

    build_fixture();
    put_u64(80U, 0x1000ULL);
    put_u64(88U, 0x1000ULL);
    expect(ELF_USER_BAD_RANGE,
          "a load address below the user image is refused");

    build_fixture();
    put_u64(24U, 0x500000ULL);
    expect(ELF_USER_BAD_ENTRY, "an entry outside an executable segment is refused");

    build_fixture();
    put_u64(104U, 0x900000ULL);
    expect(ELF_USER_BAD_RANGE, "a segment above its bound is refused");

    build_fixture();
    put_u16(56U, 2U);
    put_u32(120U, 1U);
    put_u32(124U, 5U);
    put_u64(128U, TEXT_OFFSET);
    put_u64(136U, 0x400000ULL);
    put_u64(144U, 0x400000ULL);
    put_u64(152U, 0x20U);
    put_u64(160U, 0x20U);
    put_u64(168U, 0x1000U);
    expect(ELF_USER_BAD_OVERLAP, "overlapping load segments are refused");

    build_fixture();
    put_u16(56U, 0U);
    expect(ELF_USER_NO_SEGMENTS, "an image with no program headers is refused");
}

static void test_map(void)
{
    struct elf_user_image image;
    struct address_space address_space;
    const uint8_t *mapped;

    build_fixture();
    reset_mocks();
    memset(&address_space, 0, sizeof(address_space));
    CHECK(elf_user_parse(fixture, sizeof(fixture), &image) == ELF_USER_OK,
          "the mapping fixture parses");
    CHECK(elf_user_map(&image, fixture, sizeof(fixture), &address_space, 0U) ==
              ELF_USER_OK,
          "the valid image maps");
    CHECK(mock_map_count == 1U && mock_free_count == 0U,
          "one page is mapped and none freed");
    mapped = (const uint8_t *)(uintptr_t)mock_physical[0];
    CHECK(mapped[0] == 0x90U && mapped[1] == 0U && mapped[0xFFFU] == 0U,
          "the file byte is copied and the rest of the page is zero");
    CHECK(mock_virtual[0] == 0x400000ULL,
          "the page is mapped at the segment address");

    reset_mocks();
    mock_alloc_fail_after = 0U;
    CHECK(elf_user_map(&image, fixture, sizeof(fixture), &address_space, 0U) ==
              ELF_USER_IO,
          "an allocation failure is reported");
    CHECK(mock_map_count == 0U && mock_free_count == 0U,
          "nothing is published when the first allocation fails");
}

int main(void)
{
    test_valid();
    test_refusals();
    test_map();
    if (failures != 0) {
        fprintf(stderr, "ELF user host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("ELF user host test: ALL OK\n");
    return 0;
}
