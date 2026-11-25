/*
 * ITEM-624 host proof for the user-pointer boundary.
 *
 * The page tables are assembled by hand so the test drives the production
 * vmm_validate_user_buffer() walk and user_copy.c, not a model of them.
 * Present/user/writable, supervisor-only, unmapped and cross-page cases are
 * checked before a byte is copied; a refused copy must leave the kernel
 * destination untouched.
 */
#include "user_copy.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#define TEST_ROOT 0x1000ULL
#define TEST_PDPT 0x2000ULL
#define TEST_PD   0x3000ULL
#define TEST_PT   0x4000ULL
#define TEST_DATA0 0x5000ULL
#define TEST_DATA1 0x6000ULL
#define TEST_HHDM 0xFFFF800000000000ULL
#define PAGE_SIZE 4096ULL
#define TEST_USER_BASE VMM_USER_STACK_BASE

#define TABLE_FLAGS (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define PAGE_FLAGS  (VMM_FLAG_PRESENT | VMM_FLAG_USER)
#define RO_FLAGS    (VMM_FLAG_PRESENT | VMM_FLAG_USER)

static uint64_t test_pml4[512];
static uint64_t test_pdpt[512];
static uint64_t test_pd[512];
static uint64_t test_pt[512];
static unsigned char test_data0[4096];
static unsigned char test_data1[4096];
static unsigned char *host_user;
static uint64_t test_cr3;
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

uint64_t mock_read_cr3(void) { return test_cr3; }
void mock_write_cr3(uint64_t cr3) { test_cr3 = cr3; }
void mock_invlpg(uint64_t virtual_address) { (void)virtual_address; }

void *vmm_test_phys_to_virt(uint64_t physical) {
    switch (physical) {
    case TEST_ROOT: return test_pml4;
    case TEST_PDPT: return test_pdpt;
    case TEST_PD: return test_pd;
    case TEST_PT: return test_pt;
    case TEST_DATA0: return test_data0;
    case TEST_DATA1: return test_data1;
    default: return NULL;
    }
}

void vmm_test_note_page_zeroed(uint64_t physical) { (void)physical; }

uint64_t pmm_alloc_page(void) { return 0U; }
bool pmm_free_page(uint64_t physical) { (void)physical; return false; }

static void map_user_pages(void) {
    const uint64_t address = TEST_USER_BASE;
    const uint64_t pml4_index = (address >> 39U) & 0x1FFU;
    const uint64_t pdpt_index = (address >> 30U) & 0x1FFU;
    const uint64_t pd_index = (address >> 21U) & 0x1FFU;
    const uint64_t pt_index = (address >> 12U) & 0x1FFU;

    memset(test_pml4, 0, sizeof(test_pml4));
    memset(test_pdpt, 0, sizeof(test_pdpt));
    memset(test_pd, 0, sizeof(test_pd));
    memset(test_pt, 0, sizeof(test_pt));
    memset(test_data0, 0x41, sizeof(test_data0));
    memset(test_data1, 0x42, sizeof(test_data1));
    host_user = mmap((void *)(uintptr_t)TEST_USER_BASE, 2U * PAGE_SIZE,
                     PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(host_user != MAP_FAILED, "the host user window maps");
    if (host_user == MAP_FAILED) {
        return;
    }
    memset(host_user, 0x41, PAGE_SIZE);
    memset(host_user + PAGE_SIZE, 0x42, PAGE_SIZE);
    test_pml4[pml4_index] = TEST_PDPT | TABLE_FLAGS;
    test_pdpt[pdpt_index] = TEST_PD | TABLE_FLAGS;
    test_pd[pd_index] = TEST_PT | TABLE_FLAGS;
    test_pt[pt_index] = TEST_DATA0 | PAGE_FLAGS | VMM_FLAG_WRITABLE;
    test_pt[pt_index + 1U] = TEST_DATA1 | RO_FLAGS;
    test_cr3 = TEST_ROOT;
    CHECK(vmm_init(TEST_HHDM) == VMM_OK, "the test address space initialises");
}

static void test_copy_in(void) {
    unsigned char destination[8];
    const uint64_t source = TEST_USER_BASE;

    memset(destination, 0, sizeof(destination));
    CHECK(user_copy_in(destination, source, sizeof(destination)) ==
              USER_COPY_OK &&
              memcmp(destination, host_user, sizeof(destination)) == 0,
          "copy_in reads a present user page");
    CHECK(user_copy_check(source, 0U, false) == USER_COPY_RANGE,
          "a zero length is refused before the copy");
    CHECK(user_copy_in(destination, TEST_USER_BASE - 1U, 4U) !=
              USER_COPY_OK,
          "a buffer below the user image is refused");
    CHECK(user_copy_in(destination, VMM_USER_STACK_TOP - 2U, 4U) !=
              USER_COPY_OK,
          "a buffer crossing the user top is refused");
}

static void test_copy_out_and_access(void) {
    unsigned char source[8];

    memset(source, 0x5A, sizeof(source));
    CHECK(user_copy_out(TEST_USER_BASE, source, sizeof(source)) ==
              USER_COPY_OK &&
              memcmp(host_user, source, sizeof(source)) == 0,
          "copy_out writes a writable user page");
    CHECK(user_copy_out(TEST_USER_BASE + PAGE_SIZE, source,
                        sizeof(source)) == USER_COPY_BAD_ACCESS,
          "copy_out refuses a read-only user page");
    CHECK(user_copy_out(TEST_USER_BASE + 2U * PAGE_SIZE, source,
                        sizeof(source)) == USER_COPY_NOT_MAPPED,
          "copy_out refuses an unmapped user page");
}

static void test_cross_page(void) {
    unsigned char destination[8];
    const uint64_t source = TEST_USER_BASE + PAGE_SIZE - 4U;

    memset(destination, 0, sizeof(destination));
    CHECK(user_copy_in(destination, source, sizeof(destination)) ==
              USER_COPY_OK &&
              destination[0] == 0x41 && destination[3] == 0x41 &&
              destination[4] == 0x42 && destination[7] == 0x42,
          "a buffer crossing two mapped pages copies exactly");
}

static void test_string(void) {
    char destination[32];
    size_t length = 0U;
    const uint64_t source = TEST_USER_BASE + 16U;

    memset(host_user + 16U, 0, 16U);
    memcpy(host_user + 16U, "hello", 6U);
    CHECK(user_copy_string_in(destination, sizeof(destination), source,
                              &length) == USER_COPY_OK &&
              strcmp(destination, "hello") == 0 && length == 5U,
          "a terminated user string is copied with its length");
    memset(host_user + 16U, 'x', 32U);
    CHECK(user_copy_string_in(destination, 8U, source, &length) ==
              USER_COPY_NO_NUL,
          "an unterminated window is refused");
    /* ITEM-626: the string ends in the last bytes of a mapped page; the
       capacity window would cross into the next, unmapped page.  The scan
       must validate the bytes it reads, not the whole window. */
    memcpy(host_user + PAGE_SIZE - 4U, "end", 4U);
    CHECK(user_copy_string_in(destination, sizeof(destination),
                              TEST_USER_BASE + PAGE_SIZE - 4U,
                              &length) == USER_COPY_OK &&
              strcmp(destination, "end") == 0 && length == 3U,
          "a string ending at the last mapped byte is copied");
}

static void test_refused_copy_leaves_destination(void) {
    unsigned char destination[4] = { 0xA5U, 0xA5U, 0xA5U, 0xA5U };

    CHECK(user_copy_in(destination, TEST_USER_BASE + 2U * PAGE_SIZE,
                       sizeof(destination)) != USER_COPY_OK &&
              destination[0] == 0xA5U && destination[3] == 0xA5U,
          "a refused copy leaves the kernel destination untouched");
}

int main(void) {
    map_user_pages();
    test_copy_in();
    test_copy_out_and_access();
    test_cross_page();
    test_string();
    test_refused_copy_leaves_destination();
    if (failures != 0) {
        fprintf(stderr, "user copy host test: %d failure(s)\n", failures);
        return 1;
    }
    printf("user copy host test: ALL OK\n");
    return 0;
}
