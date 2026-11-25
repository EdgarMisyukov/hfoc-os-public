#include "virtio_pci_capability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_PCI_CAPABILITY_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static void put_le16(uint8_t *buffer, size_t offset, uint16_t value) {
    buffer[offset] = (uint8_t)(value & 0xFFU);
    buffer[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void put_le32(uint8_t *buffer, size_t offset, uint32_t value) {
    put_le16(buffer, offset, (uint16_t)(value & 0xFFFFU));
    put_le16(buffer, offset + 2U,
             (uint16_t)((value >> 16U) & 0xFFFFU));
}

static void set_capability_pointer(uint8_t *config, uint16_t pointer) {
    put_le16(config, VIRTIO_PCI_CAPABILITY_POINTER_OFFSET, pointer);
}

static void install_non_vendor_capability(
    uint8_t *config,
    size_t offset,
    uint8_t next)
{
    config[offset] = 0x10U;
    config[offset + 1U] = next;
}

static void install_vendor_capability(
    uint8_t *config,
    size_t offset,
    uint8_t type,
    uint8_t next,
    uint8_t capability_length,
    uint8_t bar,
    uint32_t capability_offset,
    uint32_t capability_length_value,
    uint16_t notify_multiplier)
{
    config[offset] = VIRTIO_PCI_CAPABILITY_ID_VENDOR;
    config[offset + 1U] = next;
    config[offset + 2U] = capability_length;
    config[offset + 3U] = type;
    config[offset + 4U] = bar;
    config[offset + 5U] = 0U;
    config[offset + 6U] = 0U;
    config[offset + 7U] = 0U;
    put_le32(config, offset + 8U, capability_offset);
    put_le32(config, offset + 12U, capability_length_value);
    if (type == VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY) {
        put_le16(config, offset + 16U, notify_multiplier);
    }
}

static void make_valid_config(uint8_t *config) {
    memset(config, 0, VIRTIO_PCI_CONFIG_SPACE_MAX);
    set_capability_pointer(config, VIRTIO_PCI_FIRST_CAPABILITY_OFFSET);
    install_non_vendor_capability(config, 0x40U, 0x48U);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U,
        0x1000U, 0U);
    install_vendor_capability(
        config, 0x60U, VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY,
        0x78U, VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH, 1U,
        0x0U, 0x1000U, 4U);
    install_vendor_capability(
        config, 0x78U, VIRTIO_PCI_CAPABILITY_TYPE_ISR,
        0x88U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 2U, 0x0U,
        0x100U, 0U);
    install_vendor_capability(
        config, 0x88U, VIRTIO_PCI_CAPABILITY_TYPE_DEVICE,
        0U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 3U, 0x0U,
        0x100U, 0U);
}

static void make_valid_descending_config(uint8_t *config) {
    memset(config, 0, VIRTIO_PCI_CONFIG_SPACE_MAX);
    set_capability_pointer(config, 0x88U);
    install_vendor_capability(
        config, 0x88U, VIRTIO_PCI_CAPABILITY_TYPE_PCI_CONFIG_ACCESS,
        0x78U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH + 4U, 0U, 0x0U,
        0x100U, 0U);
    install_vendor_capability(
        config, 0x78U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U,
        0x1000U, 0U);
    install_vendor_capability(
        config, 0x60U, VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY,
        0x50U, VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH, 1U,
        0x0U, 0x1000U, 4U);
    install_vendor_capability(
        config, 0x50U, VIRTIO_PCI_CAPABILITY_TYPE_ISR,
        0x40U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 2U, 0x0U,
        0x100U, 0U);
    install_vendor_capability(
        config, 0x40U, VIRTIO_PCI_CAPABILITY_TYPE_DEVICE,
        0U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 3U, 0x0U,
        0x100U, 0U);
}

static void make_bar_ranges(
    struct virtio_pci_bar_range ranges[VIRTIO_PCI_BAR_COUNT])
{
    static const uint64_t bases[VIRTIO_PCI_BAR_COUNT] = {
        UINT64_C(0x10000000),
        UINT64_C(0x20000000),
        UINT64_C(0x30000000),
        UINT64_C(0x40000000),
        UINT64_C(0x50000000),
        UINT64_C(0x60000000),
    };
    unsigned int index;

    for (index = 0U; index < VIRTIO_PCI_BAR_COUNT; ++index) {
        ranges[index].present = true;
        ranges[index].base = bases[index];
        ranges[index].length = UINT64_C(0x10000);
    }
}

static bool status_string_matches(
    enum virtio_pci_capability_status status,
    const char *string)
{
    switch (status) {
    case VIRTIO_PCI_CAPABILITY_OK:
        return strcmp(string, "VIRTIO_PCI_CAPABILITY_OK") == 0;
    case VIRTIO_PCI_CAPABILITY_NULL:
        return strcmp(string, "VIRTIO_PCI_CAPABILITY_NULL") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH:
        return strcmp(
            string,
            "VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_POINTER:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_INVALID_POINTER") == 0;
    case VIRTIO_PCI_CAPABILITY_TRUNCATED:
        return strcmp(string, "VIRTIO_PCI_CAPABILITY_TRUNCATED") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY:
        return strcmp(
            string,
            "VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY") == 0;
    case VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_BAR:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_INVALID_BAR") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_RANGE:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_INVALID_RANGE") == 0;
    case VIRTIO_PCI_CAPABILITY_UNALIGNED:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_UNALIGNED") == 0;
    case VIRTIO_PCI_CAPABILITY_DUPLICATE:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_DUPLICATE") == 0;
    case VIRTIO_PCI_CAPABILITY_OVERLAP:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_OVERLAP") == 0;
    case VIRTIO_PCI_CAPABILITY_MISSING:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_MISSING") == 0;
    case VIRTIO_PCI_CAPABILITY_UNPUBLISHED:
        return strcmp(
            string, "VIRTIO_PCI_CAPABILITY_UNPUBLISHED") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION:
        return strcmp(
            string,
            "VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR:
        return strcmp(
            string,
            "VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR") == 0;
    case VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT:
        return strcmp(
            string,
            "VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void) {
    static const enum virtio_pci_capability_status statuses[] = {
        VIRTIO_PCI_CAPABILITY_OK,
        VIRTIO_PCI_CAPABILITY_NULL,
        VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH,
        VIRTIO_PCI_CAPABILITY_INVALID_POINTER,
        VIRTIO_PCI_CAPABILITY_TRUNCATED,
        VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY,
        VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE,
        VIRTIO_PCI_CAPABILITY_INVALID_BAR,
        VIRTIO_PCI_CAPABILITY_INVALID_RANGE,
        VIRTIO_PCI_CAPABILITY_UNALIGNED,
        VIRTIO_PCI_CAPABILITY_DUPLICATE,
        VIRTIO_PCI_CAPABILITY_OVERLAP,
        VIRTIO_PCI_CAPABILITY_MISSING,
        VIRTIO_PCI_CAPABILITY_UNPUBLISHED,
        VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION,
        VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR,
        VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT,
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        enum virtio_pci_capability_status status = statuses[index];
        const char *string = virtio_pci_capability_status_string(status);

        CHECK(string != NULL && status_string_matches(status, string),
              "virtio PCI capability status string is exact");
    }
    CHECK(virtio_pci_capability_status_string(
              (enum virtio_pci_capability_status)17U) == NULL,
          "virtio PCI capability undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "virtio PCI capability status handling performs no allocation");
}

static void test_null_and_length(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;

    memset(&result, 0, sizeof(result));
    make_bar_ranges(bars);
    make_valid_config(config);

    CHECK(virtio_pci_capability_parse(
              NULL, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_NULL,
          "capability parse rejects null config space");
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), NULL, &result) ==
              VIRTIO_PCI_CAPABILITY_NULL,
          "capability parse rejects null BAR ranges");
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, NULL) ==
              VIRTIO_PCI_CAPABILITY_NULL,
          "capability parse rejects null result");

    CHECK(virtio_pci_capability_parse(
              config, VIRTIO_PCI_CONFIG_HEADER_LENGTH - 1U,
              bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH,
          "capability parse rejects short config space");
    CHECK(virtio_pci_capability_parse(
              config, VIRTIO_PCI_CONFIG_SPACE_MAX + 1U,
              bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH,
          "capability parse rejects overlong config space");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "capability parse null/length checks perform no allocation");
}

static void test_capability_chain(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;

    make_bar_ranges(bars);
    memset(&result, 0, sizeof(result));

    make_valid_config(config);
    set_capability_pointer(config, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_MISSING,
          "zero capability pointer is missing");

    make_valid_config(config);
    set_capability_pointer(config, 0x20U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_POINTER,
          "below-header capability pointer is rejected");

    make_valid_config(config);
    set_capability_pointer(config, 0x100U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_POINTER,
          "out-of-buffer capability pointer is rejected");

    make_valid_descending_config(config);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_OK,
          "backward capability chain is accepted");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x40U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x40U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U,
        0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_POINTER,
          "cyclic capability chain is rejected");

    make_valid_config(config);
    CHECK(virtio_pci_capability_parse(
              config, 0x41U, bars, &result) ==
              VIRTIO_PCI_CAPABILITY_TRUNCATED,
          "truncated capability structure is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x40U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x48U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH - 1U, 0U,
        0x0U, 0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY,
          "short vendor capability is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x40U, VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY,
        0x48U, VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH - 1U,
        1U, 0x0U, 0x100U, 4U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY,
          "short notify capability is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x40U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x48U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH + 16U, 0U,
        0x0U, 0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, 0x50U, bars, &result) ==
              VIRTIO_PCI_CAPABILITY_TRUNCATED,
          "capability length beyond config is truncated");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x40U, 0U, 0x48U,
        VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U, 0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE,
          "zero capability type is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x40U, 7U, 0x48U,
        VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U, 0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE,
          "unknown capability type is rejected");

    make_valid_descending_config(config);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_OK,
          "optional pci config access capability is skipped");

    memset(config, 0, sizeof(config));
    set_capability_pointer(config, 0x40U);
    install_non_vendor_capability(config, 0x40U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_MISSING,
          "non-vendor-only capability list is missing");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "capability chain checks perform no allocation");
}

static void test_bar_and_range(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;

    make_bar_ranges(bars);
    memset(&result, 0, sizeof(result));

    make_valid_config(config);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 6U, 0x0U,
        0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_BAR,
          "invalid BAR number is rejected");

    make_valid_config(config);
    bars[0].present = false;
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_BAR,
          "absent referenced BAR is rejected");

    make_bar_ranges(bars);
    make_valid_config(config);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U,
        0U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_RANGE,
          "zero capability length is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U,
        UINT32_C(0xFFFFFFFC), 8U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_RANGE,
          "capability range overflow is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U,
        0x10000U, 0x1000U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_RANGE,
          "capability range outside BAR is rejected");

    make_bar_ranges(bars);
    bars[0].base = UINT64_MAX;
    bars[0].length = 2U;
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_RANGE,
          "overflowing BAR range is rejected");

    make_bar_ranges(bars);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U,
        1U, 0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_UNALIGNED,
          "unaligned capability offset is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U,
        0x0U, 0x101U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_UNALIGNED,
          "unaligned capability length is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "BAR/range checks perform no allocation");
}

static void test_duplicate_and_overlap(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;

    make_bar_ranges(bars);
    memset(&result, 0, sizeof(result));

    make_valid_config(config);
    config[0x89U] = 0xA0U;
    install_vendor_capability(
        config, 0xA0U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 4U, 0x0U,
        0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_DUPLICATE,
          "duplicate capability type is rejected");

    make_valid_config(config);
    install_vendor_capability(
        config, 0x78U, VIRTIO_PCI_CAPABILITY_TYPE_ISR,
        0x88U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U,
        0x800U, 0x900U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_OVERLAP,
          "overlapping same-BAR capabilities are rejected");

    make_valid_config(config);
    bars[1].base = bars[0].base;
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_OVERLAP,
          "cross-BAR capability alias is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "duplicate/overlap checks perform no allocation");
}

static void test_missing_capability(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;

    make_bar_ranges(bars);
    memset(&result, 0, sizeof(result));
    memset(config, 0, sizeof(config));
    set_capability_pointer(config, 0x40U);
    install_vendor_capability(
        config, 0x40U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x50U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U,
        0x100U, 0U);
    install_vendor_capability(
        config, 0x50U, VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY,
        0x60U, VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH, 1U,
        0x0U, 0x100U, 4U);
    install_vendor_capability(
        config, 0x60U, VIRTIO_PCI_CAPABILITY_TYPE_ISR,
        0U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 2U, 0x0U,
        0x100U, 0U);
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_MISSING,
          "missing device capability is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "missing capability check performs no allocation");
}

static void test_valid_parse(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;
    struct virtio_pci_capability_parse_result original;

    make_bar_ranges(bars);
    make_valid_config(config);
    memset(&result, 0xA5, sizeof(result));
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_OK,
          "valid capability list parses");
    CHECK(virtio_pci_capability_result_published(&result),
          "valid parse publishes result");
    CHECK(result.version == VIRTIO_PCI_CAPABILITY_RESULT_VERSION &&
              result.count == VIRTIO_PCI_CAPABILITY_SLOT_COUNT,
          "valid parse publishes version and four slots");
    CHECK(result.descriptors[0].type ==
              VIRTIO_PCI_CAPABILITY_TYPE_COMMON &&
              result.descriptors[0].bar == 0U &&
              result.descriptors[0].offset == 0x0U &&
              result.descriptors[0].length == 0x1000U,
          "common descriptor is exact");
    CHECK(result.descriptors[1].type ==
              VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY &&
              result.descriptors[1].bar == 1U &&
              result.descriptors[1].offset == 0x0U &&
              result.descriptors[1].length == 0x1000U &&
              result.descriptors[1].notify_offset_multiplier == 4U,
          "notify descriptor is exact");
    CHECK(result.descriptors[2].type ==
              VIRTIO_PCI_CAPABILITY_TYPE_ISR &&
              result.descriptors[2].bar == 2U &&
              result.descriptors[2].offset == 0x0U &&
              result.descriptors[2].length == 0x100U,
          "ISR descriptor is exact");
    CHECK(result.descriptors[3].type ==
              VIRTIO_PCI_CAPABILITY_TYPE_DEVICE &&
              result.descriptors[3].bar == 3U &&
              result.descriptors[3].offset == 0x0U &&
              result.descriptors[3].length == 0x100U,
          "device descriptor is exact");
    CHECK(result.snapshot.version ==
              VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION &&
              result.snapshot.capability_mask ==
                  VIRTIO_PCI_CAPABILITY_MASK,
          "valid parse publishes full capability snapshot");
    original = result;
    CHECK(virtio_pci_capability_result_validate(&result) ==
              VIRTIO_PCI_CAPABILITY_OK &&
              memcmp(&result, &original, sizeof(result)) == 0,
          "valid parse result validates without mutation");
    CHECK(virtio_pci_capability_result_published(NULL) == false &&
              virtio_pci_capability_result_published(&original),
          "published predicate handles null and published results");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid parse performs no allocation");
}

static void test_no_mutation_on_failure(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;
    struct virtio_pci_capability_parse_result original;

    make_bar_ranges(bars);
    make_valid_config(config);
    install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 6U, 0x0U,
        0x100U, 0U);
    memset(&result, 0xA5, sizeof(result));
    original = result;
    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_INVALID_BAR &&
              memcmp(&result, &original, sizeof(result)) == 0,
          "failed parse leaves destination unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "failed parse performs no allocation");
}

static void test_result_validation(void) {
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result result;
    struct virtio_pci_capability_parse_result bad;
    struct virtio_pci_capability_parse_result original;

    make_bar_ranges(bars);
    make_valid_config(config);
    memset(&result, 0, sizeof(result));

    CHECK(virtio_pci_capability_result_validate(NULL) ==
              VIRTIO_PCI_CAPABILITY_NULL,
          "result validation rejects null result");
    CHECK(virtio_pci_capability_result_validate(&result) ==
              VIRTIO_PCI_CAPABILITY_UNPUBLISHED,
          "result validation rejects unpublished result");

    CHECK(virtio_pci_capability_parse(
              config, sizeof(config), bars, &result) ==
              VIRTIO_PCI_CAPABILITY_OK,
          "result validation fixture parses");
    original = result;

    bad = result;
    bad.version = VIRTIO_PCI_CAPABILITY_RESULT_VERSION + 1U;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION,
          "result validation rejects bad version");

    bad = result;
    bad.count = VIRTIO_PCI_CAPABILITY_SLOT_COUNT - 1U;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR,
          "result validation rejects bad descriptor count");

    bad = result;
    bad.descriptors[0].type = VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR,
          "result validation rejects duplicate descriptor type");

    bad = result;
    bad.descriptors[0].bar = VIRTIO_PCI_BAR_COUNT;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR,
          "result validation rejects invalid descriptor BAR");

    bad = result;
    bad.descriptors[0].offset = 1U;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR,
          "result validation rejects unaligned descriptor offset");

    bad = result;
    bad.descriptors[0].length = 0U;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR,
          "result validation rejects zero descriptor length");

    bad = result;
    bad.snapshot.capability_mask = 1U << 4U;
    CHECK(virtio_pci_capability_result_validate(&bad) ==
              VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT,
          "result validation rejects invalid capability snapshot");

    CHECK(virtio_pci_capability_result_validate(&result) ==
              VIRTIO_PCI_CAPABILITY_OK &&
              memcmp(&result, &original, sizeof(result)) == 0,
          "valid result validates without mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "result validation performs no allocation");
}

int main(void) {
    test_status_strings();
    test_null_and_length();
    test_capability_chain();
    test_bar_and_range();
    test_duplicate_and_overlap();
    test_missing_capability();
    test_valid_parse();
    test_no_mutation_on_failure();
    test_result_validation();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio PCI capability host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Virtio PCI capability host tests: ALL OK");
    return 0;
}

#else
#error "This host test must be built with HFOC_VIRTIO_PCI_CAPABILITY_HOST_TEST"
#endif
