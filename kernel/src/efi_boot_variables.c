#include "efi_boot_variables.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EFI_BOOT_VARIABLES_MAX_ORDER_ENTRIES 32U
#define EFI_BOOT_VARIABLES_MAX_SCAN_NUMBER 31U
#define EFI_BOOT_VARIABLES_ATTRIBUTES_ACTIVE 0x00000001U

static const unsigned char efi_boot_global_guid[16U] = {
    0x61U, 0xDFU, 0xE4U, 0x8BU, 0xCAU, 0x93U, 0xD2U, 0x11U,
    0xAAU, 0x0DU, 0x00U, 0xE0U, 0x98U, 0x03U, 0x2BU, 0x8CU
};

static uint16_t efi_boot_variables_load_le16(
    const unsigned char *bytes)
{
    return (uint16_t)bytes[0U] |
           (uint16_t)((uint16_t)bytes[1U] << 8U);
}

static void efi_boot_variables_store_le16(
    unsigned char *bytes,
    uint16_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)(value >> 8U);
}

static void efi_boot_variables_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static bool efi_boot_variables_key_equal(
    const struct efi_boot_variable_key *left,
    const struct efi_boot_variable_key *right)
{
    size_t index;

    for (index = 0U;
         index < EFI_BOOT_VARIABLE_NAME_MAX;
         ++index) {
        if (left->name[index] != right->name[index]) {
            return false;
        }
        if (left->name[index] == '\0') {
            break;
        }
    }
    return memcmp(left->guid, right->guid,
                  EFI_BOOT_VARIABLE_GUID_LENGTH) == 0;
}

static char efi_boot_variables_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789ABCDEF";

    return digits[value & 0x0FU];
}

static void efi_boot_variables_boot_key(
    uint16_t number,
    struct efi_boot_variable_key *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->guid, efi_boot_global_guid,
           EFI_BOOT_VARIABLE_GUID_LENGTH);
    key->name[0U] = 'B';
    key->name[1U] = 'o';
    key->name[2U] = 'o';
    key->name[3U] = 't';
    key->name[4U] = efi_boot_variables_hex_digit(
        (unsigned int)(number >> 12U));
    key->name[5U] = efi_boot_variables_hex_digit(
        (unsigned int)((number >> 8U) & 0x0FU));
    key->name[6U] = efi_boot_variables_hex_digit(
        (unsigned int)((number >> 4U) & 0x0FU));
    key->name[7U] = efi_boot_variables_hex_digit(
        (unsigned int)(number & 0x0FU));
}

static void efi_boot_variables_boot_order_key(
    struct efi_boot_variable_key *key)
{
    memset(key, 0, sizeof(*key));
    memcpy(key->guid, efi_boot_global_guid,
           EFI_BOOT_VARIABLE_GUID_LENGTH);
    memcpy(key->name, "BootOrder", 10U);
}

static enum efi_boot_variables_status
efi_boot_variables_map_read(
    enum efi_boot_variable_io_status status)
{
    switch (status) {
    case EFI_BOOT_VARIABLE_IO_OK:
        return EFI_BOOT_VARIABLES_OK;
    case EFI_BOOT_VARIABLE_IO_NOT_FOUND:
        return EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE;
    case EFI_BOOT_VARIABLE_IO_ACCESS_DENIED:
        return EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE;
    case EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL:
        return EFI_BOOT_VARIABLES_CAPACITY;
    case EFI_BOOT_VARIABLE_IO_ERROR:
        return EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR;
    default:
        return EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR;
    }
}

static enum efi_boot_variables_status
efi_boot_variables_map_write(
    enum efi_boot_variable_io_status status)
{
    switch (status) {
    case EFI_BOOT_VARIABLE_IO_OK:
        return EFI_BOOT_VARIABLES_OK;
    case EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL:
        return EFI_BOOT_VARIABLES_CAPACITY;
    case EFI_BOOT_VARIABLE_IO_NOT_FOUND:
    case EFI_BOOT_VARIABLE_IO_ACCESS_DENIED:
    case EFI_BOOT_VARIABLE_IO_ERROR:
    default:
        return EFI_BOOT_VARIABLES_MUTATION_FAILED;
    }
}

static bool efi_boot_variables_order_contains(
    const unsigned char *order,
    size_t order_length,
    uint16_t number)
{
    size_t index;

    for (index = 0U; index < order_length; index += 2U) {
        if (efi_boot_variables_load_le16(order + index) ==
            number) {
            return true;
        }
    }
    return false;
}

static bool efi_boot_variables_snapshot_has_number(
    const struct efi_boot_variables_snapshot *snapshot,
    uint16_t number)
{
    uint32_t index;

    for (index = 0U; index < snapshot->entry_count; ++index) {
        if (snapshot->entries[index].present &&
            snapshot->entries[index].key.name[0U] == 'B' &&
            snapshot->entries[index].key.name[1U] == 'o' &&
            snapshot->entries[index].key.name[2U] == 'o' &&
            snapshot->entries[index].key.name[3U] == 't') {
            unsigned int parsed = 0U;
            size_t digit;

            for (digit = 4U; digit < 8U; ++digit) {
                char value =
                    snapshot->entries[index].key.name[digit];
                unsigned int nibble;

                if (value >= '0' && value <= '9') {
                    nibble = (unsigned int)(value - '0');
                } else if (value >= 'A' && value <= 'F') {
                    nibble = (unsigned int)(value - 'A' + 10U);
                } else {
                    nibble = 0U;
                }
                parsed = (parsed << 4U) | nibble;
            }
            if ((uint16_t)parsed == number) {
                return true;
            }
        }
    }
    return false;
}

static enum efi_boot_variables_status
efi_boot_variables_append_utf16(
    unsigned char *data,
    size_t offset,
    size_t capacity,
    const char *text)
{
    size_t index;

    if (data == NULL || text == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (offset + 2U > capacity) {
            return EFI_BOOT_VARIABLES_CAPACITY;
        }
        data[offset++] = (unsigned char)text[index];
        data[offset++] = 0U;
    }
    if (offset + 2U > capacity) {
        return EFI_BOOT_VARIABLES_CAPACITY;
    }
    data[offset++] = 0U;
    data[offset++] = 0U;
    return EFI_BOOT_VARIABLES_OK;
}

static size_t efi_boot_variables_build_hfoc_data(
    const struct efi_boot_environment *environment,
    unsigned char *data,
    size_t capacity)
{
    static const char path[] = "\\EFI\\HFOC\\BOOTX64.EFI";
    static const char description[] = "HFOC OS";
    size_t path_characters = sizeof(path) - 1U;
    size_t device_path_length;
    size_t device_path_payload_length;
    uint16_t file_path_length =
        (uint16_t)(4U + (path_characters + 1U) * 2U);
    uint32_t path_list_length;
    size_t offset = 0U;
    size_t index;

    if (environment == NULL || data == NULL ||
        environment->boot_device_path == NULL ||
        environment->boot_device_path_length < 8U ||
        environment->boot_device_path_length >
            EFI_BOOT_DEVICE_PATH_MAX ||
        environment->boot_device_path[
            environment->boot_device_path_length - 4U] != 0x7FU ||
        environment->boot_device_path[
            environment->boot_device_path_length - 3U] != 0xFFU ||
        environment->boot_device_path[
            environment->boot_device_path_length - 2U] != 0x04U ||
        environment->boot_device_path[
            environment->boot_device_path_length - 1U] != 0x00U) {
        return 0U;
    }
    device_path_length = environment->boot_device_path_length;
    device_path_payload_length = device_path_length - 4U;
    path_list_length = (uint32_t)device_path_payload_length +
        (uint32_t)file_path_length + 4U;
    if (path_list_length > UINT16_MAX ||
        capacity < 6U + (sizeof(description) - 1U) * 2U + 2U +
            path_list_length) {
        return 0U;
    }

    memset(data, 0, capacity);
    efi_boot_variables_store_le32(data + offset,
        EFI_BOOT_VARIABLES_ATTRIBUTES_ACTIVE);
    offset += 4U;
    efi_boot_variables_store_le16(data + offset,
        (uint16_t)path_list_length);
    offset += 2U;
    (void)efi_boot_variables_append_utf16(
        data, offset, capacity, description);
    offset += (sizeof(description) - 1U) * 2U + 2U;

    memcpy(data + offset, environment->boot_device_path,
           device_path_payload_length);
    offset += device_path_payload_length;
    data[offset++] = 0x04U;
    data[offset++] = 0x04U;
    efi_boot_variables_store_le16(data + offset,
        file_path_length);
    offset += 2U;
    for (index = 0U; index <= path_characters; ++index) {
        data[offset++] = (unsigned char)path[index];
        data[offset++] = 0U;
    }
    data[offset++] = 0x7FU;
    data[offset++] = 0xFFU;
    data[offset++] = 0x04U;
    data[offset++] = 0x00U;
    return offset;
}

static bool efi_boot_variables_data_is_hfoc(
    const unsigned char *data,
    size_t length)
{
    static const unsigned char expected[16U] = {
        0x48U, 0x00U, 0x46U, 0x00U,
        0x4FU, 0x00U, 0x43U, 0x00U,
        0x20U, 0x00U, 0x4FU, 0x00U,
        0x53U, 0x00U, 0x00U, 0x00U
    };

    return length >= 22U &&
           memcmp(data + 6U, expected, sizeof(expected)) == 0;
}

static uint16_t efi_boot_variables_entry_number(
    const struct efi_boot_variable_entry *entry)
{
    unsigned int number = 0U;
    size_t index;

    for (index = 4U; index < 8U; ++index) {
        char value = entry->key.name[index];
        unsigned int nibble;

        if (value >= '0' && value <= '9') {
            nibble = (unsigned int)(value - '0');
        } else if (value >= 'A' && value <= 'F') {
            nibble = (unsigned int)(value - 'A' + 10U);
        } else {
            nibble = 0U;
        }
        number = (number << 4U) | nibble;
    }
    return (uint16_t)number;
}

static enum efi_boot_variables_status
efi_boot_variables_snapshot_add(
    struct efi_boot_variables_snapshot *snapshot,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    uint32_t index;

    if (snapshot == NULL || key == NULL || data == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    if (length == 0U || length > EFI_BOOT_VARIABLES_DATA_MAX) {
        return EFI_BOOT_VARIABLES_CAPACITY;
    }
    for (index = 0U; index < snapshot->entry_count; ++index) {
        if (snapshot->entries[index].present &&
            efi_boot_variables_key_equal(
                &snapshot->entries[index].key, key)) {
            return EFI_BOOT_VARIABLES_AMBIGUOUS;
        }
    }
    if (snapshot->entry_count >=
        EFI_BOOT_VARIABLES_ENTRY_CAPACITY) {
        return EFI_BOOT_VARIABLES_CAPACITY;
    }
    index = snapshot->entry_count++;
    snapshot->entries[index].present = true;
    snapshot->entries[index].key = *key;
    snapshot->entries[index].length = length;
    memcpy(snapshot->entries[index].data, data, length);
    return EFI_BOOT_VARIABLES_OK;
}

enum efi_boot_variables_status efi_boot_variables_snapshot(
    const struct efi_boot_environment *environment,
    struct efi_boot_variables_snapshot *snapshot)
{
    struct efi_boot_variable_key key;
    struct efi_boot_variable_key boot_key;
    enum efi_boot_variable_io_status io_status;
    enum efi_boot_environment_status env_status;
    enum efi_boot_variables_status status;
    unsigned char data[EFI_BOOT_VARIABLES_DATA_MAX];
    size_t length;
    size_t order_index;
    size_t order_bytes;
    uint16_t number;
    uint16_t scan_number;

    if (environment == NULL || snapshot == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    env_status = efi_boot_environment_validate(environment);
    if (env_status != EFI_BOOT_ENVIRONMENT_OK) {
        return EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = EFI_BOOT_VARIABLES_VERSION;

    efi_boot_variables_boot_order_key(&key);
    length = 0U;
    io_status = environment->variable_read(
        environment->variable_context, &key,
        data, sizeof(data), &length);
    if (io_status != EFI_BOOT_VARIABLE_IO_NOT_FOUND) {
        status = efi_boot_variables_map_read(io_status);
        if (status != EFI_BOOT_VARIABLES_OK) {
            return status;
        }
        if (length == 0U ||
            length > EFI_BOOT_VARIABLES_ORDER_CAPACITY ||
            (length & 1U) != 0U ||
            length / 2U > EFI_BOOT_VARIABLES_MAX_ORDER_ENTRIES) {
            return EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE;
        }
        snapshot->boot_order_present = true;
        snapshot->boot_order_length = length;
        memcpy(snapshot->boot_order, data, length);
        order_bytes = snapshot->boot_order_length;

        for (order_index = 0U;
             order_index < order_bytes;
             order_index += 2U) {
            number = efi_boot_variables_load_le16(
                snapshot->boot_order + order_index);
            if (number > EFI_BOOT_VARIABLES_MAX_SCAN_NUMBER) {
                return EFI_BOOT_VARIABLES_CAPACITY;
            }
            efi_boot_variables_boot_key(number, &boot_key);
            length = 0U;
            io_status = environment->variable_read(
                environment->variable_context, &boot_key,
                data, sizeof(data), &length);
            status = efi_boot_variables_map_read(io_status);
            if (status != EFI_BOOT_VARIABLES_OK) {
                return status;
            }
            status = efi_boot_variables_snapshot_add(
                snapshot, &boot_key, data, length);
            if (status != EFI_BOOT_VARIABLES_OK) {
                return status;
            }
        }
    }

    for (scan_number = 0U;
         scan_number <= EFI_BOOT_VARIABLES_MAX_SCAN_NUMBER;
         ++scan_number) {
        if (efi_boot_variables_snapshot_has_number(
                snapshot, scan_number)) {
            continue;
        }
        efi_boot_variables_boot_key(scan_number, &boot_key);
        length = 0U;
        io_status = environment->variable_read(
            environment->variable_context, &boot_key,
            data, sizeof(data), &length);
        if (io_status == EFI_BOOT_VARIABLE_IO_NOT_FOUND) {
            continue;
        }
        status = efi_boot_variables_map_read(io_status);
        if (status != EFI_BOOT_VARIABLES_OK) {
            return status;
        }
        status = efi_boot_variables_snapshot_add(
            snapshot, &boot_key, data, length);
        if (status != EFI_BOOT_VARIABLES_OK) {
            return status;
        }
    }
    return EFI_BOOT_VARIABLES_OK;
}

enum efi_boot_variables_status efi_boot_variables_plan(
    const struct efi_boot_environment *environment,
    struct efi_boot_variables_plan *plan)
{
    /* The snapshot is larger than the 16 KiB kernel thread stack. */
    static struct efi_boot_variables_snapshot snapshot;
    enum efi_boot_variables_status status;
    const struct efi_boot_variable_entry *hfoc_entry = NULL;
    uint32_t hfoc_count = 0U;
    uint32_t index;
    uint16_t candidate;
    uint16_t lowest_free = 0U;
    bool found_free = false;

    if (environment == NULL || plan == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    status = efi_boot_variables_snapshot(environment, &snapshot);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return status;
    }
    memset(plan, 0, sizeof(*plan));
    plan->version = EFI_BOOT_VARIABLES_VERSION;
    plan->boot_order_was_present = snapshot.boot_order_present;
    plan->before_order_length = snapshot.boot_order_length;
    memcpy(plan->before_order, snapshot.boot_order,
           snapshot.boot_order_length);

    for (index = 0U; index < snapshot.entry_count; ++index) {
        const struct efi_boot_variable_entry *entry =
            &snapshot.entries[index];
        if (entry->present &&
            efi_boot_variables_data_is_hfoc(
                entry->data, entry->length)) {
            ++hfoc_count;
            hfoc_entry = entry;
        }
    }
    if (hfoc_count > 1U) {
        return EFI_BOOT_VARIABLES_AMBIGUOUS;
    }
    if (hfoc_entry != NULL) {
        plan->hfoc_present = true;
        plan->hfoc_number =
            efi_boot_variables_entry_number(hfoc_entry);
    } else {
        for (candidate = 0U;
             candidate <= EFI_BOOT_VARIABLES_MAX_SCAN_NUMBER;
             ++candidate) {
            if (!efi_boot_variables_snapshot_has_number(
                    &snapshot, candidate)) {
                lowest_free = candidate;
                found_free = true;
                break;
            }
        }
        if (!found_free) {
            return EFI_BOOT_VARIABLES_CAPACITY;
        }
        plan->hfoc_number = lowest_free;
    }
    plan->hfoc_data_length = efi_boot_variables_build_hfoc_data(
        environment, plan->hfoc_data, sizeof(plan->hfoc_data));
    if (plan->hfoc_data_length == 0U ||
        plan->hfoc_data_length > sizeof(plan->hfoc_data)) {
        return EFI_BOOT_VARIABLES_CAPACITY;
    }
    if (hfoc_entry != NULL &&
        hfoc_entry->length == plan->hfoc_data_length &&
        memcmp(hfoc_entry->data, plan->hfoc_data,
               plan->hfoc_data_length) == 0) {
        plan->hfoc_entry_change_required = false;
    } else {
        plan->hfoc_entry_change_required = true;
    }
    plan->hfoc_in_boot_order =
        snapshot.boot_order_present &&
        efi_boot_variables_order_contains(
            snapshot.boot_order, snapshot.boot_order_length,
            plan->hfoc_number);
    plan->boot_order_change_required =
        !plan->hfoc_in_boot_order;
    return EFI_BOOT_VARIABLES_OK;
}

enum efi_boot_variables_status efi_boot_variables_confirm(
    struct efi_boot_variables_plan *plan)
{
    if (plan == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    if (plan->version != EFI_BOOT_VARIABLES_VERSION) {
        return EFI_BOOT_VARIABLES_BAD_STATE;
    }
    if (plan->confirmed) {
        return EFI_BOOT_VARIABLES_BAD_STATE;
    }
    if (plan->hfoc_data_length == 0U ||
        plan->hfoc_data_length > EFI_BOOT_VARIABLES_DATA_MAX) {
        return EFI_BOOT_VARIABLES_BAD_STATE;
    }
    plan->confirmed = true;
    return EFI_BOOT_VARIABLES_OK;
}

enum efi_boot_variables_status efi_boot_variables_apply(
    const struct efi_boot_environment *environment,
    const struct efi_boot_variables_snapshot *snapshot,
    const struct efi_boot_variables_plan *plan)
{
    struct efi_boot_variable_key key;
    struct efi_boot_variable_key order_key;
    enum efi_boot_variable_io_status io_status;
    enum efi_boot_environment_status env_status;
    enum efi_boot_variables_status status;
    unsigned char new_order[EFI_BOOT_VARIABLES_ORDER_CAPACITY];
    size_t new_order_length = 0U;

    if (environment == NULL || snapshot == NULL || plan == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    env_status = efi_boot_environment_validate(environment);
    if (env_status != EFI_BOOT_ENVIRONMENT_OK) {
        return EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT;
    }
    if (snapshot->version != EFI_BOOT_VARIABLES_VERSION ||
        plan->version != EFI_BOOT_VARIABLES_VERSION) {
        return EFI_BOOT_VARIABLES_INVALID_SNAPSHOT;
    }
    if (!plan->confirmed) {
        return EFI_BOOT_VARIABLES_NOT_CONFIRMED;
    }
    if (plan->hfoc_data_length == 0U ||
        plan->hfoc_data_length > EFI_BOOT_VARIABLES_DATA_MAX) {
        return EFI_BOOT_VARIABLES_BAD_STATE;
    }

    if (plan->hfoc_entry_change_required) {
        efi_boot_variables_boot_key(
            plan->hfoc_number, &key);
        io_status = environment->variable_write(
            environment->variable_context, &key,
            plan->hfoc_data, plan->hfoc_data_length);
        status = efi_boot_variables_map_write(io_status);
        if (status != EFI_BOOT_VARIABLES_OK) {
            return status;
        }
    }
    if (!plan->boot_order_change_required) {
        return EFI_BOOT_VARIABLES_OK;
    }

    if (snapshot->boot_order_present) {
        if (snapshot->boot_order_length >
            EFI_BOOT_VARIABLES_ORDER_CAPACITY - 2U) {
            return EFI_BOOT_VARIABLES_CAPACITY;
        }
        efi_boot_variables_store_le16(
            new_order + new_order_length,
            plan->hfoc_number);
        new_order_length += 2U;
        memcpy(new_order + new_order_length,
               snapshot->boot_order,
               snapshot->boot_order_length);
        new_order_length += snapshot->boot_order_length;
    } else {
        efi_boot_variables_store_le16(
            new_order + new_order_length,
            plan->hfoc_number);
        new_order_length += 2U;
    }
    if (new_order_length > EFI_BOOT_VARIABLES_ORDER_CAPACITY) {
        return EFI_BOOT_VARIABLES_CAPACITY;
    }
    efi_boot_variables_boot_order_key(&order_key);
    io_status = environment->variable_write(
        environment->variable_context, &order_key,
        new_order, new_order_length);
    status = efi_boot_variables_map_write(io_status);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return status;
    }
    return EFI_BOOT_VARIABLES_OK;
}

enum efi_boot_variables_status efi_boot_variables_verify(
    const struct efi_boot_environment *environment,
    const struct efi_boot_variables_plan *plan)
{
    struct efi_boot_variable_key key;
    struct efi_boot_variable_key order_key;
    enum efi_boot_variable_io_status io_status;
    enum efi_boot_environment_status env_status;
    enum efi_boot_variables_status status;
    unsigned char data[EFI_BOOT_VARIABLES_DATA_MAX];
    unsigned char order[EFI_BOOT_VARIABLES_ORDER_CAPACITY];
    size_t length;
    size_t order_length;
    bool expected_order_equal = false;

    if (environment == NULL || plan == NULL) {
        return EFI_BOOT_VARIABLES_NULL;
    }
    env_status = efi_boot_environment_validate(environment);
    if (env_status != EFI_BOOT_ENVIRONMENT_OK) {
        return EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT;
    }
    if (plan->version != EFI_BOOT_VARIABLES_VERSION ||
        plan->hfoc_data_length == 0U ||
        plan->hfoc_data_length > EFI_BOOT_VARIABLES_DATA_MAX) {
        return EFI_BOOT_VARIABLES_BAD_STATE;
    }

    efi_boot_variables_boot_key(plan->hfoc_number, &key);
    length = 0U;
    io_status = environment->variable_read(
        environment->variable_context, &key,
        data, sizeof(data), &length);
    status = efi_boot_variables_map_read(io_status);
    if (status != EFI_BOOT_VARIABLES_OK ||
        length != plan->hfoc_data_length ||
        memcmp(data, plan->hfoc_data, length) != 0) {
        return EFI_BOOT_VARIABLES_VERIFICATION_FAILED;
    }

    efi_boot_variables_boot_order_key(&order_key);
    order_length = 0U;
    io_status = environment->variable_read(
        environment->variable_context, &order_key,
        order, sizeof(order), &order_length);
    status = efi_boot_variables_map_read(io_status);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return EFI_BOOT_VARIABLES_VERIFICATION_FAILED;
    }
    if (order_length == 0U ||
        order_length > EFI_BOOT_VARIABLES_ORDER_CAPACITY ||
        (order_length & 1U) != 0U) {
        return EFI_BOOT_VARIABLES_VERIFICATION_FAILED;
    }

    if (!plan->boot_order_was_present) {
        expected_order_equal =
            order_length == 2U &&
            efi_boot_variables_load_le16(order) ==
                plan->hfoc_number;
    } else if (plan->hfoc_in_boot_order) {
        expected_order_equal =
            order_length == plan->before_order_length &&
            memcmp(order, plan->before_order,
                   order_length) == 0;
    } else {
        expected_order_equal =
            order_length == plan->before_order_length + 2U &&
            efi_boot_variables_load_le16(order) ==
                plan->hfoc_number &&
            memcmp(order + 2U, plan->before_order,
                   plan->before_order_length) == 0;
    }
    if (!expected_order_equal) {
        return EFI_BOOT_VARIABLES_VERIFICATION_FAILED;
    }
    return EFI_BOOT_VARIABLES_OK;
}

const char *efi_boot_variables_status_string(
    enum efi_boot_variables_status status)
{
    switch (status) {
    case EFI_BOOT_VARIABLES_OK:
        return "EFI_BOOT_VARIABLES_OK";
    case EFI_BOOT_VARIABLES_NULL:
        return "EFI_BOOT_VARIABLES_NULL";
    case EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT:
        return "EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT";
    case EFI_BOOT_VARIABLES_INVALID_SNAPSHOT:
        return "EFI_BOOT_VARIABLES_INVALID_SNAPSHOT";
    case EFI_BOOT_VARIABLES_BAD_STATE:
        return "EFI_BOOT_VARIABLES_BAD_STATE";
    case EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE:
        return "EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE";
    case EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR:
        return "EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR";
    case EFI_BOOT_VARIABLES_CAPACITY:
        return "EFI_BOOT_VARIABLES_CAPACITY";
    case EFI_BOOT_VARIABLES_AMBIGUOUS:
        return "EFI_BOOT_VARIABLES_AMBIGUOUS";
    case EFI_BOOT_VARIABLES_NOT_CONFIRMED:
        return "EFI_BOOT_VARIABLES_NOT_CONFIRMED";
    case EFI_BOOT_VARIABLES_MUTATION_FAILED:
        return "EFI_BOOT_VARIABLES_MUTATION_FAILED";
    case EFI_BOOT_VARIABLES_VERIFICATION_FAILED:
        return "EFI_BOOT_VARIABLES_VERIFICATION_FAILED";
    default:
        return NULL;
    }
}
