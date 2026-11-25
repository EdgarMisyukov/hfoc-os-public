#ifndef HFOC_APPLIANCE_GPT_DISCOVERY_H
#define HFOC_APPLIANCE_GPT_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

#define APPLIANCE_GPT_BLOCK_SIZE 512U
#define APPLIANCE_GPT_HEADER_LBA 1U
#define APPLIANCE_GPT_HEADER_SIZE 92U
#define APPLIANCE_GPT_ENTRY_SIZE 128U
#define APPLIANCE_GPT_ENTRY_COUNT_MAX 128U
#define APPLIANCE_GPT_PARTITION_CAPACITY 64U

enum appliance_gpt_discovery_status {
    APPLIANCE_GPT_DISCOVERY_OK = 0,
    APPLIANCE_GPT_DISCOVERY_NULL = 1,
    APPLIANCE_GPT_DISCOVERY_BAD_DEVICE = 2,
    APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE = 3,
    APPLIANCE_GPT_DISCOVERY_IO_ERROR = 4,
    APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE = 5,
    APPLIANCE_GPT_DISCOVERY_BAD_HEADER = 6,
    APPLIANCE_GPT_DISCOVERY_BAD_ENTRY = 7,
    APPLIANCE_GPT_DISCOVERY_OVERLAP = 8,
    APPLIANCE_GPT_DISCOVERY_AMBIGUOUS = 9,
    APPLIANCE_GPT_DISCOVERY_NOT_FOUND = 10,
    APPLIANCE_GPT_DISCOVERY_OUT_OF_RANGE = 11,
    APPLIANCE_GPT_DISCOVERY_CAPACITY = 12,
};

struct appliance_gpt_hfoc_partition {
    bool present;
    uint32_t partition_index;
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t block_count;
};

enum appliance_gpt_discovery_status appliance_gpt_find_hfoc_partition(
    struct block_device *device,
    struct appliance_gpt_hfoc_partition *partition);
enum appliance_gpt_discovery_status appliance_gpt_validate_hfoc_partition(
    const struct appliance_gpt_hfoc_partition *partition);
/*
 * ITEM-410: the `index`-th EFI System Partition of the table, in GPT slot
 * order (0 is the first).  A machine can carry several ESPs - one per disk,
 * one per installed system - and the appliance boot log has to find the one
 * that belongs to the install it is running from, so a caller walks them and
 * asks each extent what it holds.  Read-only: it reports the extent and writes
 * nothing.
 */
enum appliance_gpt_discovery_status appliance_gpt_find_esp_partition(
    struct block_device *device,
    uint32_t index,
    struct appliance_gpt_hfoc_partition *partition);
const char *appliance_gpt_discovery_status_string(
    enum appliance_gpt_discovery_status status);

#endif
