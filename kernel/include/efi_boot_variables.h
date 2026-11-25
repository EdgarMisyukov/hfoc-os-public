#ifndef HFOC_EFI_BOOT_VARIABLES_H
#define HFOC_EFI_BOOT_VARIABLES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "efi_boot_environment.h"

#define EFI_BOOT_VARIABLES_VERSION 1U
#define EFI_BOOT_VARIABLES_ENTRY_CAPACITY 32U
#define EFI_BOOT_VARIABLES_DATA_MAX 512U
#define EFI_BOOT_VARIABLES_ORDER_CAPACITY 128U

enum efi_boot_variables_status {
    EFI_BOOT_VARIABLES_OK = 0,
    EFI_BOOT_VARIABLES_NULL = 1,
    EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT = 2,
    EFI_BOOT_VARIABLES_INVALID_SNAPSHOT = 3,
    EFI_BOOT_VARIABLES_BAD_STATE = 4,
    EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE = 5,
    EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR = 6,
    EFI_BOOT_VARIABLES_CAPACITY = 7,
    EFI_BOOT_VARIABLES_AMBIGUOUS = 8,
    EFI_BOOT_VARIABLES_NOT_CONFIRMED = 9,
    EFI_BOOT_VARIABLES_MUTATION_FAILED = 10,
    EFI_BOOT_VARIABLES_VERIFICATION_FAILED = 11,
};

struct efi_boot_variable_entry {
    bool present;
    struct efi_boot_variable_key key;
    size_t length;
    unsigned char data[EFI_BOOT_VARIABLES_DATA_MAX];
};

struct efi_boot_variables_snapshot {
    uint32_t version;
    bool boot_order_present;
    size_t boot_order_length;
    unsigned char boot_order[EFI_BOOT_VARIABLES_ORDER_CAPACITY];
    uint32_t entry_count;
    struct efi_boot_variable_entry
        entries[EFI_BOOT_VARIABLES_ENTRY_CAPACITY];
};

struct efi_boot_variables_plan {
    uint32_t version;
    bool confirmed;
    bool hfoc_present;
    bool hfoc_in_boot_order;
    bool boot_order_was_present;
    bool boot_order_change_required;
    bool hfoc_entry_change_required;
    uint16_t hfoc_number;
    size_t before_order_length;
    unsigned char before_order[EFI_BOOT_VARIABLES_ORDER_CAPACITY];
    size_t hfoc_data_length;
    unsigned char hfoc_data[EFI_BOOT_VARIABLES_DATA_MAX];
};

enum efi_boot_variables_status efi_boot_variables_snapshot(
    const struct efi_boot_environment *environment,
    struct efi_boot_variables_snapshot *snapshot);
enum efi_boot_variables_status efi_boot_variables_plan(
    const struct efi_boot_environment *environment,
    struct efi_boot_variables_plan *plan);
enum efi_boot_variables_status efi_boot_variables_confirm(
    struct efi_boot_variables_plan *plan);
enum efi_boot_variables_status efi_boot_variables_apply(
    const struct efi_boot_environment *environment,
    const struct efi_boot_variables_snapshot *snapshot,
    const struct efi_boot_variables_plan *plan);
enum efi_boot_variables_status efi_boot_variables_verify(
    const struct efi_boot_environment *environment,
    const struct efi_boot_variables_plan *plan);
const char *efi_boot_variables_status_string(
    enum efi_boot_variables_status status);

#endif
