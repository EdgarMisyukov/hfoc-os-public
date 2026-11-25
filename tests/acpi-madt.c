#include "acpi.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* Test wrappers from kernel/src/acpi.c (compiled with -DHFOC_ACPI_HOST_TEST). */
extern bool acpi_test_is_physical_range_mapped(
    uint64_t phys_base, uint64_t length,
    const struct limine_memmap_response *memory_map);
#ifdef HFOC_ACPI_HOST_TEST
extern bool acpi_test_phys_range_to_virt(
    uint64_t phys, uint64_t length, uint64_t hhdm_offset);
extern enum acpi_status acpi_test_parse_madt(
    const void *madt_virtual, uint32_t madt_length,
    struct acpi_madt_info *out);
#endif

static int failures = 0;

#define TEST(name) do { printf("  %s: ", name); } while (0)
#define PASS() do { puts("PASS"); } while (0)
#define FAIL(msg) do { puts("FAIL: " msg); ++failures; } while (0)
#define CHECK(cond, msg) do { if (!(cond)) { FAIL(msg); return; } } while (0)

/* --- Synthetic table buffers --- */
static uint8_t rsdp_v2[36];
static uint8_t xsdt[68];
static uint8_t rsdt[52];
static uint8_t madt[44];
static uint8_t non_apic_sdt[36 + 8];
static uint8_t bad_csum_madt[44];
static uint8_t short_madt[40];
static uint8_t apic2_madt[44];
static uint8_t unaligned_buf[128];
static uint8_t trailing_madt[256];
/* ITEM-411: FADT fixtures.  The reset register lives at offset 116..128 and
   the flags word at 112, so 132 bytes carry every field this kernel reads. */
static uint8_t fadt[132];
static uint8_t fadt_second[132];

static struct limine_memmap_entry mem_entries[8];
static struct limine_memmap_entry *mem_ptrs[8];
static struct limine_memmap_response mem_map;

/* --- Checksum helpers --- */
static void checksum_sdt(uint8_t *table, uint32_t length) {
    table[9] = 0U;
    uint8_t s = 0; for (uint32_t i = 0; i < length; ++i) s += table[i];
    table[9] = (uint8_t)((0U - (unsigned)s) & 0xFFU);
}

static void checksum_region(uint8_t *buf, size_t len, size_t csum_off) {
    buf[csum_off] = 0U;
    uint8_t s = 0; for (size_t i = 0; i < len; ++i) s += buf[i];
    buf[csum_off] = (uint8_t)((0U - (unsigned)s) & 0xFFU);
}

static void write_sdt_header(uint8_t *table, const char sig[4],
                             uint32_t length, uint8_t revision) {
    memcpy(table, sig, 4);
    memcpy(table + 4, &length, 4);
    table[8] = revision;
    table[9] = 0;
    memcpy(table + 10, "HFOCOS", 6);
    memcpy(table + 16, "MADT-TBL", 8);
    memset(table + 24, 0, 12);
    checksum_sdt(table, length);
}

/* --- Builders --- */
static void build_madt(void) {
    memset(madt, 0, sizeof(madt));
    write_sdt_header(madt, "APIC", 44, 1);
    { uint32_t a = 0xFEE00000U; memcpy(madt + 36, &a, 4); }
    { uint32_t f = 1U; memcpy(madt + 40, &f, 4); }
    checksum_sdt(madt, 44);
}

static void build_xsdt(void) {
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 68, 1);
    { uint64_t a = (uint64_t)(uintptr_t)madt; memcpy(xsdt + 36, &a, 8); }
    checksum_sdt(xsdt, 68);
}

static void build_rsdt(void) {
    memset(rsdt, 0, sizeof(rsdt));
    write_sdt_header(rsdt, "RSDT", 52, 1);
    { uint32_t a = (uint32_t)(uintptr_t)madt; memcpy(rsdt + 36, &a, 4); }
    checksum_sdt(rsdt, 52);
}

static void build_rsdp_v2(void) {
    memset(rsdp_v2, 0, sizeof(rsdp_v2));
    memcpy(rsdp_v2, "RSD PTR ", 8);
    memcpy(rsdp_v2 + 9, "HFOCOS", 6);
    rsdp_v2[15] = 2;
    rsdp_v2[20] = 36;
    { uint32_t a = (uint32_t)(uintptr_t)rsdt; memcpy(rsdp_v2 + 16, &a, 4); }
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
}

/* --- Memory map covering ALL buffers --- */
static void setup_memory_map(void) {
    uintptr_t addrs[] = {
        (uintptr_t)rsdp_v2, (uintptr_t)xsdt, (uintptr_t)rsdt,
        (uintptr_t)madt, (uintptr_t)non_apic_sdt,
        (uintptr_t)bad_csum_madt, (uintptr_t)short_madt,
        (uintptr_t)apic2_madt, (uintptr_t)unaligned_buf,
        (uintptr_t)trailing_madt, (uintptr_t)fadt, (uintptr_t)fadt_second
    };
    size_t sizes[] = {
        sizeof(rsdp_v2), sizeof(xsdt), sizeof(rsdt),
        sizeof(madt), sizeof(non_apic_sdt),
        sizeof(bad_csum_madt), sizeof(short_madt),
        sizeof(apic2_madt), sizeof(unaligned_buf),
        sizeof(trailing_madt), sizeof(fadt), sizeof(fadt_second)
    };
    uintptr_t lo = addrs[0], hi = addrs[0] + sizes[0];
    for (size_t i = 1; i < sizeof(addrs)/sizeof(addrs[0]); ++i) {
        if (addrs[i] < lo) lo = addrs[i];
        if (addrs[i] + sizes[i] > hi) hi = addrs[i] + sizes[i];
    }
    uint64_t range_base = (uint64_t)(lo & ~0xFFFULL);
    uint64_t range_len  = (uint64_t)((hi + 0xFFFULL) & ~0xFFFULL)
                          - range_base + 0x1000;
    mem_entries[0] = (struct limine_memmap_entry){
        .base = range_base, .length = range_len,
        .type = LIMINE_MEMMAP_ACPI_RECLAIMABLE };
    mem_ptrs[0] = &mem_entries[0];
    mem_map.entries = mem_ptrs;
    mem_map.entry_count = 1;
}

/* --- Range mapping tests --- */
static void test_range_single(void) {
    TEST("single entry covers range");
    mem_entries[0] = (struct limine_memmap_entry){
        0x1000, 0x3000, LIMINE_MEMMAP_ACPI_RECLAIMABLE };
    mem_ptrs[0] = &mem_entries[0];
    mem_map.entries = mem_ptrs; mem_map.entry_count = 1;
    CHECK(acpi_test_is_physical_range_mapped(0x1000, 0x3000, &mem_map),
          "range should be mapped");
    PASS();
}

static void test_range_two_adjacent(void) {
    TEST("two adjacent entries cover range");
    mem_entries[0] = (struct limine_memmap_entry){
        0x1000, 0x1000, LIMINE_MEMMAP_ACPI_RECLAIMABLE };
    mem_entries[1] = (struct limine_memmap_entry){
        0x2000, 0x1000, LIMINE_MEMMAP_ACPI_NVS };
    mem_ptrs[0] = &mem_entries[0]; mem_ptrs[1] = &mem_entries[1];
    mem_map.entries = mem_ptrs; mem_map.entry_count = 2;
    CHECK(acpi_test_is_physical_range_mapped(0x1000, 0x2000, &mem_map),
          "adjacent should cover range");
    PASS();
}

static void test_range_gap(void) {
    TEST("gap between entries fails");
    mem_entries[0] = (struct limine_memmap_entry){
        0x1000, 0xFFF, LIMINE_MEMMAP_ACPI_RECLAIMABLE };
    mem_entries[1] = (struct limine_memmap_entry){
        0x3000, 0x1000, LIMINE_MEMMAP_ACPI_NVS };
    mem_ptrs[0] = &mem_entries[0]; mem_ptrs[1] = &mem_entries[1];
    mem_map.entries = mem_ptrs; mem_map.entry_count = 2;
    CHECK(!acpi_test_is_physical_range_mapped(0x1000, 0x3000, &mem_map),
          "gap should fail");
    PASS();
}

static void test_range_bad_type(void) {
    TEST("bad type fails");
    mem_entries[0] = (struct limine_memmap_entry){
        0x1000, 0x1000, LIMINE_MEMMAP_USABLE };
    mem_ptrs[0] = &mem_entries[0];
    mem_map.entries = mem_ptrs; mem_map.entry_count = 1;
    CHECK(!acpi_test_is_physical_range_mapped(0x1000, 0x1000, &mem_map),
          "usable type should not cover ACPI range");
    PASS();
}

static void test_range_overflow_request(void) {
    TEST("request overflow fails");
    mem_entries[0] = (struct limine_memmap_entry){
        0x1000, 0x1000, LIMINE_MEMMAP_ACPI_RECLAIMABLE };
    mem_ptrs[0] = &mem_entries[0];
    mem_map.entries = mem_ptrs; mem_map.entry_count = 1;
    CHECK(!acpi_test_is_physical_range_mapped(
              UINT64_MAX - 0x500, 0x1000, &mem_map),
          "overflow request should fail");
    PASS();
}

static void test_range_overflow_entry(void) {
    TEST("entry overflow ignored safely");
    mem_entries[0] = (struct limine_memmap_entry){
        0x1000, UINT64_MAX - 0x500,
        LIMINE_MEMMAP_ACPI_RECLAIMABLE };
    mem_ptrs[0] = &mem_entries[0];
    mem_map.entries = mem_ptrs; mem_map.entry_count = 1;
    CHECK(!acpi_test_is_physical_range_mapped(0x1000, 0x2000, &mem_map),
          "overflow entry should not match");
    PASS();
}

/* --- RSDP / root-table tests (all modify static buffers in-place) --- */
static void test_rsdp_bad_signature(void) {
    TEST("bad RSDP signature rejected");
    build_madt(); build_xsdt(); build_rsdt(); build_rsdp_v2();
    rsdp_v2[0] = (uint8_t)'X';
    checksum_region(rsdp_v2, 20, 8);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out)
          == ACPI_BAD_RSDP_SIGNATURE, "bad sig not detected");
    PASS();
}

static void test_rsdp_legacy_checksum_fail(void) {
    TEST("RSDP legacy checksum failure");
    build_madt(); build_xsdt(); build_rsdt(); build_rsdp_v2();
    rsdp_v2[9] = (uint8_t)(rsdp_v2[9] + 1U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out)
          == ACPI_RSDP_LEGACY_CHECKSUM, "bad legacy csum not detected");
    PASS();
}

static void test_rsdp_revision_1_rejected(void) {
    TEST("RSDP revision 1 rejected");
    build_madt(); build_xsdt(); build_rsdt(); build_rsdp_v2();
    rsdp_v2[15] = 1;
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out)
          == ACPI_UNSUPPORTED_RSDP_REVISION, "rev 1 not rejected");
    PASS();
}

static void test_rsdp_extended_range_before_checksum(void) {
    TEST("extended RSDP: range checked before checksum");
    build_madt(); build_xsdt(); build_rsdt(); build_rsdp_v2();
    rsdp_v2[20] = 0xFF;
    rsdp_v2[21] = 0xFF;
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st != ACPI_OK, "oversized RSDP accepted");
    PASS();
}

static void test_xsdt_to_rsdt_fallback(void) {
    TEST("invalid XSDT -> valid RSDT fallback (Task 3: ACPI_OK)");
    build_madt(); build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "BAD!", 68, 1);
    checksum_sdt(xsdt, 68);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    { uint32_t a = (uint32_t)(uintptr_t)rsdt; memcpy(rsdp_v2 + 16, &a, 4); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_OK, "fallback should return ACPI_OK (MADT parsed via RSDT)");
    CHECK(out.root_source == ACPI_ROOT_RSDT, "root should be RSDT");
    PASS();
}

static void test_bad_sdt_signature(void) {
    TEST("bad SDT signature rejected");
    build_madt(); build_xsdt(); build_rsdt(); build_rsdp_v2();
    xsdt[0] = (uint8_t)'B';
    checksum_sdt(xsdt, 68);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out)
          == ACPI_OK, "fallback to RSDT should succeed");
    PASS();
}

static void test_bad_sdt_divisibility(void) {
    TEST("bad SDT payload divisibility rejected");
    build_madt(); build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 37, 1);
    checksum_sdt(xsdt, 37);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out)
          == ACPI_OK, "fallback to RSDT should succeed");
    PASS();
}

static void test_rsdp_hhdm_underflow(void) {
    TEST("RSDP below HHDM offset rejected");
    build_madt(); build_xsdt(); build_rsdt(); build_rsdp_v2();
    setup_memory_map();
    uint64_t huge = (uint64_t)(uintptr_t)rsdp_v2 + 1ULL;
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, huge, &out)
          != ACPI_OK, "underflow not detected");
    PASS();
}

/* --- Root-table fallback error tests --- */

static void test_root_fallback_no_rsdt(void) {
    /* invalid XSDT + RSDT address == 0 -> ACPI_NO_VALID_ROOT_TABLE */
    TEST("invalid XSDT + no RSDT -> ACPI_NO_VALID_ROOT_TABLE");
    build_madt(); build_rsdt(); build_rsdp_v2();
    /* Corrupt XSDT signature. */
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "BAD!", 68, 1);
    checksum_sdt(xsdt, 68);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    /* Zero out RSDT address in RSDP. */
    memset(rsdp_v2 + 16, 0, 4);
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_NO_VALID_ROOT_TABLE,
          "should return ACPI_NO_VALID_ROOT_TABLE when XSDT invalid and RSDT absent");
    { const uint8_t *p = (const uint8_t *)&out; size_t nz = 0;
      for (size_t i = 0; i < sizeof(out); ++i) nz += p[i];
      CHECK(nz == 0, "out should be fully zeroed"); }
    PASS();
}

static void test_root_fallback_both_invalid(void) {
    /* invalid XSDT + invalid RSDT -> ACPI_NO_VALID_ROOT_TABLE */
    TEST("invalid XSDT + invalid RSDT -> ACPI_NO_VALID_ROOT_TABLE");
    build_madt(); build_rsdt(); build_rsdp_v2();
    /* Corrupt XSDT signature. */
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "BAD!", 68, 1);
    checksum_sdt(xsdt, 68);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    /* Corrupt RSDT signature. */
    memset(rsdt, 0, sizeof(rsdt));
    write_sdt_header(rsdt, "BAD!", 52, 1);
    checksum_sdt(rsdt, 52);
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_NO_VALID_ROOT_TABLE,
          "should return ACPI_NO_VALID_ROOT_TABLE when both tables invalid");
    { const uint8_t *p = (const uint8_t *)&out; size_t nz = 0;
      for (size_t i = 0; i < sizeof(out); ++i) nz += p[i];
      CHECK(nz == 0, "out should be fully zeroed"); }
    PASS();
}

/* --- find_madt behaviour tests --- */
static void build_non_apic_sdt(void) {
    memset(non_apic_sdt, 0, sizeof(non_apic_sdt));
    write_sdt_header(non_apic_sdt, "FACP", 44, 1);
}

static void build_bad_csum_madt(void) {
    memcpy(bad_csum_madt, madt, 44);
    bad_csum_madt[9] = (uint8_t)(bad_csum_madt[9] + 1U);
}

static void build_short_madt(void) {
    memset(short_madt, 0, sizeof(short_madt));
    write_sdt_header(short_madt, "APIC", 40, 1);
}

static void build_apic2_madt(void) {
    memcpy(apic2_madt, madt, 44);
}

static void test_find_madt_non_apic_skip(void) {
    TEST("non-APIC root entry skipped");
    build_madt(); build_non_apic_sdt(); build_rsdt(); build_rsdp_v2();
    /* Replace XSDT entry 0: non-APIC, entry 1: MADT. */
    memset(xsdt, 0, sizeof(xsdt));
    { uint32_t len = 36 + 16; write_sdt_header(xsdt, "XSDT", len, 1); }
    { uint64_t a = (uint64_t)(uintptr_t)non_apic_sdt; memcpy(xsdt + 36, &a, 8); }
    { uint64_t a = (uint64_t)(uintptr_t)madt; memcpy(xsdt + 44, &a, 8); }
    checksum_sdt(xsdt, 36 + 16);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    { enum acpi_status _st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
      CHECK(_st == ACPI_OK, "non-APIC skip + valid MADT -> ACPI_OK"); }
    PASS();
}

static void test_find_madt_bad_checksum(void) {
    TEST("APIC bad checksum returns ACPI_SDT_CHECKSUM");
    build_madt(); build_bad_csum_madt(); build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 44, 1);
    { uint64_t a = (uint64_t)(uintptr_t)bad_csum_madt; memcpy(xsdt + 36, &a, 8); }
    checksum_sdt(xsdt, 44);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out)
          == ACPI_SDT_CHECKSUM, "bad checksum not detected");
    PASS();
}

static void test_find_madt_short_madt(void) {
    /* Task 2: validate_sdt accepts length >= 36.  The minimum MADT length
     * (>= 44) is enforced by acpi_test_parse_madt() in Task 3.  A short-but-valid
     * MADT therefore passes find_madt, and acpi_parse returns ACPI_NO_MADT. */
    TEST("APIC length < 44 accepted by validate, returns ACPI_NO_MADT");
    build_madt(); build_short_madt(); build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 44, 1);
    { uint64_t a = (uint64_t)(uintptr_t)short_madt; memcpy(xsdt + 36, &a, 8); }
    checksum_sdt(xsdt, 44);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    { enum acpi_status _st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
      CHECK(_st == ACPI_BAD_SDT_LENGTH, "short MADT should return ACPI_BAD_SDT_LENGTH in Task 3"); }
    PASS();
}

static void test_find_madt_duplicate(void) {
    TEST("duplicate APIC returns ACPI_DUPLICATE_MADT");
    build_madt(); build_apic2_madt(); build_rsdt(); build_rsdp_v2();
    { uint32_t len = 52;
      memset(xsdt, 0, sizeof(xsdt));
      write_sdt_header(xsdt, "XSDT", len, 1);
      { uint64_t a = (uint64_t)(uintptr_t)madt; memcpy(xsdt + 36, &a, 8); }
      { uint64_t a = (uint64_t)(uintptr_t)apic2_madt; memcpy(xsdt + 44, &a, 8); }
      checksum_sdt(xsdt, len); }
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    { enum acpi_status _st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
      CHECK(_st == ACPI_DUPLICATE_MADT, "duplicate MADT not detected"); }
    PASS();
}

static void test_unaligned_xsdt_payload(void) {
    TEST("unaligned XSDT payload read byte-wise");
    build_madt(); build_rsdt(); build_rsdp_v2();
    memset(unaligned_buf, 0, sizeof(unaligned_buf));
    uint8_t *uxsdt = unaligned_buf + 1;
    write_sdt_header(uxsdt, "XSDT", 44, 1);
    { uint64_t a = (uint64_t)(uintptr_t)madt; memcpy(uxsdt + 36, &a, 8); }
    checksum_sdt(uxsdt, 44);
    { uint64_t a = (uint64_t)(uintptr_t)uxsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    { enum acpi_status _st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
      CHECK(_st == ACPI_OK, "unaligned XSDT should parse and find MADT"); }
    PASS();
}

#ifdef HFOC_ACPI_HOST_TEST
static void test_phys_range_to_virt_valid(void) {
    TEST("phys_range_to_virt: valid full range");
    CHECK(acpi_test_phys_range_to_virt(0x1000, 0x2000, 0x1000),
          "valid range should succeed");
    PASS();
}

static void test_phys_range_to_virt_overflow(void) {
    TEST("phys_range_to_virt: virtual-end overflow");
    uint64_t phys = 0x1000;
    uint64_t length = 0x2000;
    uint64_t hhdm = UINT64_MAX - phys - 0x500;
    CHECK(!acpi_test_phys_range_to_virt(phys, length, hhdm),
          "virtual-end overflow should fail");
    PASS();
}
#endif

static void test_virtual_range_overflow(void) {
    TEST("phys_range_to_virt: combined");
#ifdef HFOC_ACPI_HOST_TEST
    test_phys_range_to_virt_valid();
    test_phys_range_to_virt_overflow();
#else
    puts("SKIP (no HFOC_ACPI_HOST_TEST)");
#endif
    PASS();
}


/* --- MADT parsing tests (Task 3) --- */

/* Helper: build a custom MADT in a provided buffer.
 * entries_raw and entries_len describe the entry bytes after the 44-byte header.
 * lapic_addr_override: if non-NULL, used as the 64-bit LAPIC address
 *   (type 5 override); otherwise the header field is used. */
static void build_madt_custom(uint8_t *buf, uint32_t total_len,
                              uint32_t lapic_addr, uint32_t flags,
                              const uint8_t *entries_raw, uint32_t entries_len)
{
    memset(buf, 0, total_len);
    write_sdt_header(buf, "APIC", total_len, 1);
    { uint32_t a = lapic_addr; memcpy(buf + 36, &a, 4); }
    { uint32_t f = flags; memcpy(buf + 40, &f, 4); }
    if (entries_raw != NULL && entries_len > 0U)
        memcpy(buf + 44, entries_raw, entries_len);
    checksum_sdt(buf, total_len);
}

/* Full snapshot: 4 LAPIC, 2 IOAPIC, 2 ISO, 4 LAPIC NMI, 1 override (XSDT). */
static void test_madt_full_snapshot_xsdt(void) {
    TEST("full MADT snapshot via XSDT");
    /* Build a rich MADT. */
    static uint8_t rich_madt[256];
    uint32_t total = 44 + 4*8 + 2*12 + 2*10 + 4*6 + 1*12;
    memset(rich_madt, 0, total);
    write_sdt_header(rich_madt, "APIC", total, 1);
    { uint32_t a = 0xFEE00000U; memcpy(rich_madt + 36, &a, 4); }
    { uint32_t f = 1U; memcpy(rich_madt + 40, &f, 4); }
    uint8_t *e = rich_madt + 44;
    /* Type 0 × 4 */
    for (int i = 0; i < 4; ++i) {
        e[0]=0; e[1]=8; e[2]=(uint8_t)i; e[3]=(uint8_t)(i*2);
        { uint32_t f= (i==1)?1U:0U; memcpy(e+4,&f,4); }
        e+=8;
    }
    /* Type 1 × 2 */
    for (int i = 0; i < 2; ++i) {
        e[0]=1; e[1]=12; e[2]=(uint8_t)i; e[3]=0;
        { uint32_t a=0xFEC00000U+(uint32_t)i*0x1000; memcpy(e+4,&a,4); }
        { uint32_t g=(uint32_t)i*24; memcpy(e+8,&g,4); }
        e+=12;
    }
    /* Type 2 × 2 */
    for (int i = 0; i < 2; ++i) {
        e[0]=2; e[1]=10; e[2]=0; e[3]=(uint8_t)i;
        { uint32_t g=(uint32_t)i; memcpy(e+4,&g,4); }
        { uint16_t fl=(i==0)?2U:0U; memcpy(e+8,&fl,2); }
        e+=10;
    }
    /* Type 4 × 4 */
    for (int i = 0; i < 4; ++i) {
        e[0]=4; e[1]=6; e[2]=(uint8_t)(0xFF); /* all processors */
        { uint16_t fl=(uint16_t)(i+1); memcpy(e+3,&fl,2); }
        e[5]=(uint8_t)i;
        e+=6;
    }
    /* Type 5 × 1 (override to 0xFEE01000) */
    { e[0]=5; e[1]=12; e[2]=0; e[3]=0;
      { uint64_t a=0xFEE01000ULL; memcpy(e+4,&a,8); }
      e+=12; }
    checksum_sdt(rich_madt, total);

    build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    { uint32_t len = 44; write_sdt_header(xsdt, "XSDT", len, 1); }
    { uint64_t a = (uint64_t)(uintptr_t)rich_madt; memcpy(xsdt + 36, &a, 8); }
    checksum_sdt(xsdt, 44);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_OK, "full snapshot should return ACPI_OK");
    CHECK(out.root_source == ACPI_ROOT_XSDT, "root should be XSDT");
    CHECK(out.lapic_count == 4, "lapic count mismatch");
    CHECK(out.ioapic_count == 2, "ioapic count mismatch");
    CHECK(out.iso_count == 2, "iso count mismatch");
    CHECK(out.lapic_nmi_count == 4, "lapic nmi count mismatch");
    CHECK(out.has_override, "should have override");
    CHECK(out.has_8259, "should have 8259");
    CHECK(out.lapic_address == 0xFEE01000ULL, "lapic addr should be override");
    CHECK(out.lapics[1].flags == 1U, "lapic[1] should be disabled");
    CHECK(out.isos[0].flags == 2U, "iso[0] flags mismatch");
    PASS();
}

/* Full snapshot via RSDT (invalid XSDT -> valid RSDT). */
static void test_madt_full_snapshot_rsdt(void) {
    TEST("full MADT snapshot via RSDT");
    build_madt(); build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "BAD!", 68, 1);
    checksum_sdt(xsdt, 68);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_OK, "RSDT fallback should return ACPI_OK");
    CHECK(out.root_source == ACPI_ROOT_RSDT, "root should be RSDT");
    CHECK(out.lapic_address == 0xFEE00000U, "should use header LAPIC addr");
    PASS();
}

/* Individual type tests. */
static void test_madt_type_0(void) {
    TEST("MADT type 0 (LAPIC)");
    static uint8_t buf[44 + 8];
    uint8_t e[] = {0, 8, 3, 6, 1, 0, 0, 0};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 8);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.lapic_count == 1, "LAPIC not parsed");
    CHECK(out.lapics[0].acpi_processor_id == 3, "processor id");
    CHECK(out.lapics[0].apic_id == 6, "apic id");
    CHECK(out.lapics[0].flags == 1U, "flags");
    PASS();
}

static void test_madt_type_1(void) {
    TEST("MADT type 1 (IOAPIC)");
    static uint8_t buf[44 + 12];
    uint8_t e[] = {1, 12, 2, 0, 0, 0, 0xE0, 0xFE, 0, 0, 0, 0};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 12);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.ioapic_count == 1, "IOAPIC not parsed");
    CHECK(out.ioapics[0].ioapic_id == 2, "ioapic id");
    CHECK(out.ioapics[0].mmio_address == 0xFEE00000U, "mmio");
    PASS();
}

static void test_madt_type_2(void) {
    TEST("MADT type 2 (ISO)");
    static uint8_t buf[44 + 10];
    uint8_t e[] = {2, 10, 0, 5, 5, 0, 0, 0, 0xF, 0};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 10);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.iso_count == 1, "ISO not parsed");
    CHECK(out.isos[0].bus == 0, "bus");
    CHECK(out.isos[0].source == 5, "source");
    CHECK(out.isos[0].gsi == 5U, "gsi");
    CHECK(out.isos[0].flags == 0xFU, "flags");
    PASS();
}

static void test_madt_type_4(void) {
    TEST("MADT type 4 (LAPIC NMI)");
    static uint8_t buf[44 + 6];
    uint8_t e[] = {4, 6, 0xFF, 1, 0, 0};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 6);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.lapic_nmi_count == 1, "NMI not parsed");
    CHECK(out.lapic_nmis[0].acpi_processor_id == 0xFF, "proc id");
    CHECK(out.lapic_nmis[0].flags == 1U, "flags");
    CHECK(out.lapic_nmis[0].lint == 0, "lint");
    PASS();
}

static void test_madt_type_5(void) {
    TEST("MADT type 5 (LAPIC override)");
    static uint8_t buf[44 + 12];
    uint8_t e[] = {5, 12, 0, 0, 0x00,0x20,0xE0,0xFE, 0x00,0x00,0x00,0x00};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 12);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.has_override, "override not set");
    CHECK(out.lapic_address == 0xFEE02000ULL, "override addr");
    PASS();
}

static void test_madt_type_9(void) {
    TEST("MADT type 9 (x2APIC)");
    static uint8_t buf[44 + 16];
    uint8_t e[16];
    memset(e, 0, sizeof(e));
    e[0] = 9;
    e[1] = 16;
    memcpy(e + 4, "\x15\x00\x00\x00", 4);   /* x2APIC ID 21 */
    memcpy(e + 8, "\x01\x00\x00\x00", 4);   /* enabled */
    memcpy(e + 12, "\x07\x00\x00\x00", 4);  /* ACPI processor UID 7 */
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, sizeof(e));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.x2apic_count == 1, "x2APIC not parsed");
    CHECK(out.x2apics[0].acpi_processor_uid == 7U,
          "x2APIC processor uid");
    CHECK(out.x2apics[0].x2apic_id == 21U, "x2APIC id");
    CHECK(out.x2apics[0].flags == 1U, "x2APIC flags");
    PASS();
}

static void test_madt_type_10(void) {
    TEST("MADT type 10 (x2APIC NMI)");
    static uint8_t buf[44 + 12];
    uint8_t e[12];
    memset(e, 0, sizeof(e));
    e[0] = 10;
    e[1] = 12;
    memcpy(e + 2, "\x0F\x00", 2);           /* flags 15 */
    memcpy(e + 4, "\x07\x00\x00\x00", 4);   /* ACPI processor UID 7 */
    e[8] = 1;                               /* LINT1 */
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, sizeof(e));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.x2apic_nmi_count == 1,
          "x2APIC NMI not parsed");
    CHECK(out.x2apic_nmis[0].acpi_processor_uid == 7U,
          "x2APIC NMI processor uid");
    CHECK(out.x2apic_nmis[0].flags == 0x0FU, "x2APIC NMI flags");
    CHECK(out.x2apic_nmis[0].lint == 1U, "x2APIC NMI lint");
    PASS();
}

static void test_madt_mixed_xapic_x2apic(void) {
    TEST("MADT mixed type 0 and type 9 topology");
    static uint8_t buf[44 + 8 + 16];
    uint8_t e[24];
    memset(e, 0, sizeof(e));
    e[0] = 0; e[1] = 8; e[2] = 3; e[3] = 6;
    e[8] = 9; e[9] = 16;
    memcpy(e + 12, "\x2A\x00\x00\x00", 4);  /* x2APIC ID 42 */
    memcpy(e + 16, "\x01\x00\x00\x00", 4);  /* enabled */
    memcpy(e + 20, "\x09\x00\x00\x00", 4);  /* ACPI processor UID 9 */
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, sizeof(e));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK, "mixed topology should parse");
    CHECK(out.lapic_count == 1, "xAPIC count");
    CHECK(out.x2apic_count == 1, "x2APIC count");
    CHECK(out.lapics[0].acpi_processor_id == 3U, "xAPIC processor id");
    CHECK(out.x2apics[0].x2apic_id == 42U, "x2APIC id");
    CHECK(out.x2apics[0].acpi_processor_uid == 9U,
          "x2APIC processor uid");
    PASS();
}

static void test_madt_malformed_x2apic_entries(void) {
    TEST("MADT malformed type 9/10");
    static uint8_t buf9[44 + 15];
    uint8_t e9[15];
    memset(e9, 0, sizeof(e9));
    e9[0] = 9; e9[1] = 15; /* type 9 needs 16 */
    build_madt_custom(buf9, sizeof(buf9), 0xFEE00000U, 0U, e9, sizeof(e9));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf9, sizeof(buf9), &out)
          == ACPI_MALFORMED_ENTRY, "short type 9 not rejected");

    static uint8_t buf10[44 + 11];
    uint8_t e10[11];
    memset(e10, 0, sizeof(e10));
    e10[0] = 10; e10[1] = 11; /* type 10 needs 12 */
    build_madt_custom(buf10, sizeof(buf10), 0xFEE00000U, 0U, e10, sizeof(e10));
    memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf10, sizeof(buf10), &out)
          == ACPI_MALFORMED_ENTRY, "short type 10 not rejected");
    PASS();
}

static void test_madt_capacity_overflow_x2apic(void) {
    TEST("MADT capacity overflow: x2APIC");
    static uint8_t buf[44 + 257 * 16];
    memset(buf, 0, sizeof(buf));
    write_sdt_header(buf, "APIC", sizeof(buf), 1);
    { uint32_t a = 0xFEE00000U; memcpy(buf + 36, &a, 4); }
    uint8_t *e = buf + 44;
    for (int i = 0; i < 257; ++i) {
        e[0] = 9; e[1] = 16; e += 16;
    }
    checksum_sdt(buf, sizeof(buf));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out)
          == ACPI_CAPACITY_OVERFLOW, "x2APIC overflow not detected");
    PASS();
}

static void test_madt_capacity_overflow_x2apic_nmi(void) {
    TEST("MADT capacity overflow: x2APIC NMI");
    static uint8_t buf[44 + 33 * 12];
    memset(buf, 0, sizeof(buf));
    write_sdt_header(buf, "APIC", sizeof(buf), 1);
    { uint32_t a = 0xFEE00000U; memcpy(buf + 36, &a, 4); }
    uint8_t *e = buf + 44;
    for (int i = 0; i < 33; ++i) {
        e[0] = 10; e[1] = 12; e += 12;
    }
    checksum_sdt(buf, sizeof(buf));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out)
          == ACPI_CAPACITY_OVERFLOW, "x2APIC NMI overflow not detected");
    PASS();
}

static void test_madt_unknown_type(void) {
    TEST("MADT unknown type skipped");
    static uint8_t buf[44 + 6];
    uint8_t e[] = {0xFF, 6, 1, 2, 3, 4};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 6);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK, "unknown type should be skipped");
    CHECK(out.lapic_count == 0 && out.ioapic_count == 0, "nothing parsed");
    PASS();
}

static void test_madt_malformed_too_short(void) {
    TEST("MADT malformed: entry too short");
    static uint8_t buf[44 + 4];
    uint8_t e[] = {0, 3, 0, 0}; /* LAPIC needs 8, got 3 */
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 4);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out) == ACPI_MALFORMED_ENTRY,
          "too-short entry not rejected");
    PASS();
}

static void test_madt_malformed_past_end(void) {
    TEST("MADT malformed: entry past table end");
    static uint8_t buf[44 + 2];
    uint8_t e[] = {0, 20, 0, 0}; /* claims 20 bytes, only 2 available */
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 2);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out) == ACPI_MALFORMED_ENTRY,
          "past-end entry not rejected");
    PASS();
}

static void test_madt_capacity_overflow_lapic(void) {
    TEST("MADT capacity overflow: LAPIC");
    static uint8_t buf[44 + 257 * 8];
    memset(buf, 0, sizeof(buf));
    write_sdt_header(buf, "APIC", sizeof(buf), 1);
    { uint32_t a=0xFEE00000U; memcpy(buf+36,&a,4); }
    uint8_t *e = buf + 44;
    for (int i = 0; i < 257; ++i) { /* 257 > ACPI_MAX_LAPICS(256) */
        e[0]=0; e[1]=8; e[2]=0; e[3]=0; memset(e+4,0,4); e+=8;
    }
    checksum_sdt(buf, sizeof(buf));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out) == ACPI_CAPACITY_OVERFLOW,
          "LAPIC overflow not detected");
    PASS();
}

static void test_madt_capacity_overflow_ioapic(void) {
    TEST("MADT capacity overflow: IOAPIC");
    static uint8_t buf[44 + 17 * 12];
    memset(buf, 0, sizeof(buf));
    write_sdt_header(buf, "APIC", sizeof(buf), 1);
    { uint32_t a=0xFEE00000U; memcpy(buf+36,&a,4); }
    uint8_t *e = buf + 44;
    for (int i = 0; i < 17; ++i) { /* 17 > ACPI_MAX_IOAPICS(16) */
        e[0]=1; e[1]=12; memset(e+2,0,10); e+=12;
    }
    checksum_sdt(buf, sizeof(buf));
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out) == ACPI_CAPACITY_OVERFLOW,
          "IOAPIC overflow not detected");
    PASS();
}

static void test_madt_duplicate_override(void) {
    TEST("MADT duplicate type 5");
    static uint8_t buf[44 + 24];
    uint8_t e[] = {
        5,12,0,0, 0,0,0x10,0xFE,0,0,0,0,  /* first override */
        5,12,0,0, 0,0,0x20,0xFE,0,0,0,0   /* second override */
    };
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 24);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    CHECK(acpi_test_parse_madt(buf, sizeof(buf), &out) == ACPI_DUPLICATE_LAPIC_OVERRIDE,
          "duplicate override not detected");
    PASS();
}

static void test_madt_override_above_4gib(void) {
    TEST("MADT LAPIC override above 4 GiB");
    static uint8_t buf[44 + 12];
    uint8_t e[] = {5,12,0,0, 0x00,0x00,0x00,0x00,0x10,0x00,0x00,0x00};
    build_madt_custom(buf, sizeof(buf), 0xFEE00000U, 0U, e, 12);
    struct acpi_madt_info out; memset(&out, 0, sizeof(out));
    enum acpi_status st = acpi_test_parse_madt(buf, sizeof(buf), &out);
    CHECK(st == ACPI_OK && out.has_override, "override not parsed");
    CHECK(out.lapic_address == 0x1000000000ULL, "64-bit override addr mismatch");
    PASS();
}

static void test_madt_no_madt_in_root(void) {
    TEST("MADT absent -> ACPI_NO_MADT");
    build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 36, 1); /* zero entries */
    checksum_sdt(xsdt, 36);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_NO_MADT, "should return ACPI_NO_MADT");
    { const uint8_t *p = (const uint8_t *)&out; size_t nz = 0;
      for (size_t i = 0; i < sizeof(out); ++i) nz += p[i];
      CHECK(nz == 0, "out should be fully zeroed"); }
    PASS();
}

static void test_madt_duplicate_in_root(void) {
    TEST("duplicate MADT in root -> ACPI_DUPLICATE_MADT");
    build_madt(); build_apic2_madt(); build_rsdt(); build_rsdp_v2();
    { uint32_t len = 52;
      memset(xsdt, 0, sizeof(xsdt));
      write_sdt_header(xsdt, "XSDT", len, 1);
      { uint64_t a = (uint64_t)(uintptr_t)madt; memcpy(xsdt + 36, &a, 8); }
      { uint64_t a = (uint64_t)(uintptr_t)apic2_madt; memcpy(xsdt + 44, &a, 8); }
      checksum_sdt(xsdt, len); }
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_DUPLICATE_MADT, "duplicate MADT not detected");
    { const uint8_t *p = (const uint8_t *)&out; size_t nz = 0;
      for (size_t i = 0; i < sizeof(out); ++i) nz += p[i];
      CHECK(nz == 0, "out should be fully zeroed"); }
    PASS();
}

static void test_madt_length_less_than_44(void) {
    TEST("MADT length < 44 -> ACPI_BAD_SDT_LENGTH via parse_madt");
    build_madt(); build_rsdt(); build_rsdp_v2();
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 44, 1);
    /* Point to a 40-byte MADT that passes validate_sdt but fails parse_madt. */
    build_short_madt();
    { uint64_t a = (uint64_t)(uintptr_t)short_madt; memcpy(xsdt + 36, &a, 8); }
    checksum_sdt(xsdt, 44);
    { uint64_t a = (uint64_t)(uintptr_t)xsdt; memcpy(rsdp_v2 + 24, &a, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_BAD_SDT_LENGTH, "length < 44 not rejected by parse_madt");
    { const uint8_t *p = (const uint8_t *)&out; size_t nz = 0;
      for (size_t i = 0; i < sizeof(out); ++i) nz += p[i];
      CHECK(nz == 0, "out should be fully zeroed"); }
    PASS();
}

static void test_madt_trailing_byte(void) {
    TEST("MADT trailing byte after valid entries");
    const uint32_t total = 44U + 8U + 1U;
    uint8_t e[9];
    memset(e, 0, 9);
    e[0] = 0; /* type 0: Processor Local APIC */
    e[1] = 8; /* length */
    e[8] = 0xFF; /* trailing byte */
    build_madt_custom(trailing_madt, total, 0xFEE00000U, 0U, e, 9);
    /* Wire through XSDT for full acpi_parse zero-out verification. */
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", 44, 1);
    { uint64_t a = (uint64_t)(uintptr_t)trailing_madt; memcpy(xsdt + 36, &a, 8); }
    checksum_sdt(xsdt, 44);
    build_rsdp_v2();
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    enum acpi_status st = acpi_parse(rsdp_v2, &mem_map, 0, &out);
    CHECK(st == ACPI_MALFORMED_ENTRY,
          "trailing byte should be rejected as ACPI_MALFORMED_ENTRY");
    { const uint8_t *p = (const uint8_t *)&out; size_t nz = 0;
      for (size_t i = 0; i < sizeof(out); ++i) nz += p[i];
      CHECK(nz == 0, "out should be fully zeroed after trailing-byte error"); }
    PASS();
}

/* Stubs for acpi_init() linker requirements (Task 4). */
void serial_write(const char *s)   { (void)s; }
void serial_write_u64(uint64_t v)  { (void)v; }
void serial_write_hex(uint64_t v)  { (void)v; }
uint64_t serial_sequence_begin(void)       { return 0U; }
void serial_sequence_end(uint64_t flags)   { (void)flags; }

/* --- ITEM-411: the FADT reset register --- */

/*
 * A FADT fixture: `length` is the table length, `flags` the FADT flags word at
 * offset 112 and the rest the reset register at 116..128.  A table shorter than
 * 129 bytes leaves the register undescribed, which is what a pre-ACPI-2.0 FADT
 * looks like - and the one case where a declared reset register cannot be read.
 */
static void build_fadt(uint8_t *table, uint32_t length, uint8_t revision,
                       uint32_t flags, uint8_t space, uint8_t width,
                       uint8_t access, uint64_t address, uint8_t value) {
    memset(table, 0, sizeof(fadt));
    write_sdt_header(table, "FACP", length, revision);
    if (length >= ACPI_FADT_RESET_FLAGS_MIN_LENGTH) {
        table[112] = (uint8_t)(flags & 0xFFU);
        table[113] = (uint8_t)((flags >> 8) & 0xFFU);
        table[114] = (uint8_t)((flags >> 16) & 0xFFU);
        table[115] = (uint8_t)((flags >> 24) & 0xFFU);
    }
    if (length >= ACPI_FADT_RESET_MIN_LENGTH) {
        table[116] = space;
        table[117] = width;
        table[118] = 0U;
        table[119] = access;
        for (unsigned int i = 0U; i < 8U; ++i)
            table[120U + i] = (uint8_t)((address >> (8U * i)) & 0xFFU);
        table[128] = value;
    }
    checksum_sdt(table, length);
}

/* An XSDT pointing at the given tables, wired into the RSDP. */
static void build_root_with(const uintptr_t *tables, uint32_t count) {
    const uint32_t length = 36U + 8U * count;
    memset(xsdt, 0, sizeof(xsdt));
    write_sdt_header(xsdt, "XSDT", length, 1);
    for (uint32_t i = 0U; i < count; ++i) {
        uint64_t address = (uint64_t)tables[i];
        memcpy(xsdt + 36U + 8U * i, &address, 8);
    }
    checksum_sdt(xsdt, length);
    { uint64_t address = (uint64_t)(uintptr_t)xsdt;
      memcpy(rsdp_v2 + 24, &address, 8); }
    checksum_region(rsdp_v2, 20, 8);
    checksum_region(rsdp_v2, 36, 32);
}

static void test_fadt_reset_system_io(void) {
    TEST("FADT reset register in SystemIO is published");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 5U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    const uintptr_t tables[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(tables, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the root table with a FADT and a MADT should parse");
    const struct acpi_fadt_reset *reset = acpi_fadt_reset();
    CHECK(reset != NULL, "the FADT reset register should be published");
    CHECK(reset->present && reset->supported,
          "the declared register should be reported as supported");
    CHECK(reset->address_space == ACPI_ADDRESS_SPACE_SYSTEM_IO,
          "the address space should be SystemIO");
    CHECK(reset->bit_width == 8U && reset->access_size == 1U,
          "the register should carry the declared width and access size");
    CHECK(reset->address == 0xCF9U, "the address should be 0xCF9");
    CHECK(reset->value == 0x06U, "the value should be 0x06");
    PASS();
}

static void test_fadt_reset_system_memory(void) {
    TEST("FADT reset register in SystemMemory above 4 GiB is published");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 6U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_MEMORY, 32U, 3U,
               UINT64_C(0x1FEDF1000), 0xABU);
    const uintptr_t tables[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(tables, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the root table should parse");
    const struct acpi_fadt_reset *reset = acpi_fadt_reset();
    CHECK(reset != NULL && reset->supported,
          "the memory-space register should be published as supported");
    CHECK(reset->address_space == ACPI_ADDRESS_SPACE_SYSTEM_MEMORY,
          "the address space should be SystemMemory");
    CHECK(reset->address == UINT64_C(0x1FEDF1000),
          "a 64-bit register address should survive the parse");
    CHECK(reset->bit_width == 32U && reset->access_size == 3U,
          "the register should carry the declared width and access size");
    CHECK(reset->value == 0xABU, "the value should be 0xAB");
    PASS();
}

static void test_fadt_reset_flag_clear(void) {
    TEST("FADT without RESET_REG_SUP declares no reset register");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 5U, 0U, ACPI_ADDRESS_SPACE_SYSTEM_IO,
               8U, 1U, 0xCF9U, 0x06U);
    const uintptr_t tables[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(tables, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the root table should parse");
    const struct acpi_fadt_reset *reset = acpi_fadt_reset();
    CHECK(reset != NULL, "the FADT itself is still a FADT");
    CHECK(reset->present && !reset->supported,
          "a FADT without RESET_REG_SUP declares no reset register");
    CHECK(reset->address == 0U && reset->value == 0U,
          "the undeclared register fields should stay zero");
    PASS();
}

static void test_fadt_reset_short_table(void) {
    TEST("FADT that stops before the reset register declares none");
    build_madt(); build_rsdt(); build_rsdp_v2();
    /* 116 bytes: the flags word is there and names a register the table does
       not describe. */
    build_fadt(fadt, 116U, 3U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    const uintptr_t tables[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(tables, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the root table should parse");
    const struct acpi_fadt_reset *reset = acpi_fadt_reset();
    CHECK(reset != NULL && reset->present && !reset->supported,
          "a register the table does not describe is not usable");
    CHECK(reset->address == 0U, "no address should be invented");
    PASS();
}

static void test_fadt_reset_before_flags(void) {
    TEST("FADT shorter than the flags word declares nothing");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 100U, 1U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    const uintptr_t tables[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(tables, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the root table should parse");
    CHECK(acpi_fadt_reset() == NULL,
          "a FADT too short to carry the flags word declares nothing");
    PASS();
}

static void test_fadt_reset_bad_checksum(void) {
    TEST("FADT with a bad checksum is not a declaration");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 5U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    fadt[9] = (uint8_t)(fadt[9] ^ 0xFFU);
    const uintptr_t tables[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(tables, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the MADT should still parse");
    CHECK(acpi_fadt_reset() == NULL,
          "a FADT that fails its checksum is not published");
    PASS();
}

static void test_fadt_reset_duplicate(void) {
    TEST("duplicate FADT in the root table declares no reset register");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 5U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    build_fadt(fadt_second, 132U, 5U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x0EU);
    const uintptr_t tables[] = {
        (uintptr_t)fadt, (uintptr_t)fadt_second, (uintptr_t)madt
    };
    build_root_with(tables, 3U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the MADT should still parse");
    CHECK(acpi_fadt_reset() == NULL,
          "two power management tables do not declare one reset register");
    PASS();
}

static void test_fadt_reset_without_madt(void) {
    TEST("the reset register survives a root table with no MADT");
    build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 5U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    const uintptr_t tables[] = { (uintptr_t)fadt };
    build_root_with(tables, 1U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0xFF, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_NO_MADT,
          "a root table with no MADT is still ACPI_NO_MADT");
    const struct acpi_fadt_reset *reset = acpi_fadt_reset();
    CHECK(reset != NULL && reset->supported,
          "the platform's reset register must not be lost with the MADT");
    CHECK(reset->address == 0xCF9U && reset->value == 0x06U,
          "the declared register should be the FADT's own");
    PASS();
}

static void test_fadt_reset_cleared_before_parse(void) {
    TEST("a later parse does not inherit an earlier reset register");
    build_madt(); build_rsdt(); build_rsdp_v2();
    build_fadt(fadt, 132U, 5U, ACPI_FADT_FLAG_RESET_REG_SUP,
               ACPI_ADDRESS_SPACE_SYSTEM_IO, 8U, 1U, 0xCF9U, 0x06U);
    const uintptr_t with_fadt[] = { (uintptr_t)fadt, (uintptr_t)madt };
    build_root_with(with_fadt, 2U);
    setup_memory_map();
    struct acpi_madt_info out;
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK &&

          acpi_fadt_reset() != NULL,
          "the first parse should publish the register");
    const uintptr_t without_fadt[] = { (uintptr_t)madt };
    build_root_with(without_fadt, 1U);
    setup_memory_map();
    memset(&out, 0, sizeof(out));
    CHECK(acpi_parse(rsdp_v2, &mem_map, 0, &out) == ACPI_OK,
          "the second root table should parse");
    CHECK(acpi_fadt_reset() == NULL,
          "the second parse must not inherit the first one's FADT");
    PASS();
}

int main(void) {
    test_range_single();
    test_range_two_adjacent();
    test_range_gap();
    test_range_bad_type();
    test_range_overflow_request();
    test_range_overflow_entry();
    test_rsdp_bad_signature();
    test_rsdp_legacy_checksum_fail();
    test_rsdp_revision_1_rejected();
    test_rsdp_extended_range_before_checksum();
    test_xsdt_to_rsdt_fallback();
    test_bad_sdt_signature();
    test_bad_sdt_divisibility();
    test_rsdp_hhdm_underflow();
    test_root_fallback_no_rsdt();
    test_root_fallback_both_invalid();
    test_find_madt_non_apic_skip();
    test_find_madt_bad_checksum();
    test_find_madt_short_madt();
    test_find_madt_duplicate();
    test_unaligned_xsdt_payload();
    test_madt_full_snapshot_xsdt();
    test_madt_full_snapshot_rsdt();
    test_madt_type_0();
    test_madt_type_1();
    test_madt_type_2();
    test_madt_type_4();
    test_madt_type_5();
    test_madt_type_9();
    test_madt_type_10();
    test_madt_mixed_xapic_x2apic();
    test_madt_malformed_x2apic_entries();
    test_madt_capacity_overflow_x2apic();
    test_madt_capacity_overflow_x2apic_nmi();
    test_madt_unknown_type();
    test_madt_malformed_too_short();
    test_madt_malformed_past_end();
    test_madt_capacity_overflow_lapic();
    test_madt_capacity_overflow_ioapic();
    test_madt_duplicate_override();
    test_madt_override_above_4gib();
    test_madt_no_madt_in_root();
    test_madt_duplicate_in_root();
    test_madt_length_less_than_44();
    test_madt_trailing_byte();
    test_fadt_reset_system_io();
    test_fadt_reset_system_memory();
    test_fadt_reset_flag_clear();
    test_fadt_reset_short_table();
    test_fadt_reset_before_flags();
    test_fadt_reset_bad_checksum();
    test_fadt_reset_duplicate();
    test_fadt_reset_without_madt();
    test_fadt_reset_cleared_before_parse();
    test_virtual_range_overflow();
    if (failures == 0) {
        puts("ACPI MADT host test: ALL OK");
        return 0;
    }
    printf("ACPI MADT host test: %d FAILURES\n", failures);
    return 1;
}
