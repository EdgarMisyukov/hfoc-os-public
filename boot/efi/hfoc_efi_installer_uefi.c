#include "hfoc_efi_installer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "efi_boot_variables.h"
#include "efi_fat32_esp.h"
#include "uefi_min.h"

#define HFOC_EFI_HELPER_MAX_BLOCK_HANDLES 16U
#define HFOC_EFI_HELPER_MAX_VOLUME_HANDLES 64U
#define HFOC_EFI_HELPER_VARIABLE_ATTRIBUTES \
    (0x00000001U | 0x00000002U | 0x00000004U)

static const struct efi_guid efi_block_io_guid = {
    .data1 = 0x964E5B21U,
    .data2 = 0x6459U,
    .data3 = 0x11D2U,
    .data4 = { 0x8EU, 0x39U, 0x00U, 0xA0U,
               0xC9U, 0x69U, 0x72U, 0x3BU },
};

static const struct efi_guid efi_device_path_guid = {
    .data1 = 0x09576E91U,
    .data2 = 0x6D3FU,
    .data3 = 0x11D2U,
    .data4 = { 0x8EU, 0x39U, 0x00U, 0xA0U,
               0xC9U, 0x69U, 0x72U, 0x3BU },
};

static const struct efi_guid efi_simple_file_system_guid = {
    .data1 = 0x964E5B22U,
    .data2 = 0x6459U,
    .data3 = 0x11D2U,
    .data4 = { 0x8EU, 0x39U, 0x00U, 0xA0U,
               0xC9U, 0x69U, 0x72U, 0x3BU },
};

static const struct efi_guid efi_file_info_guid = {
    .data1 = 0x09576E92U,
    .data2 = 0x6D3FU,
    .data3 = 0x11D2U,
    .data4 = { 0x8EU, 0x39U, 0x00U, 0xA0U,
               0xC9U, 0x69U, 0x72U, 0x3BU },
};

struct hfoc_efi_block_context {
    struct efi_block_io_protocol *block_io;
    const struct efi_device_path_protocol *device_path;
    size_t device_path_length;
};

static struct hfoc_efi_block_context block_contexts[
    HFOC_EFI_HELPER_MAX_BLOCK_HANDLES];
static struct block_device block_devices[
    HFOC_EFI_HELPER_MAX_BLOCK_HANDLES];
static unsigned char payload[EFI_FAT32_ESP_PAYLOAD_MAX];
static struct efi_system_table *system_table;

static void helper_outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t helper_inb(uint16_t port)
{
    uint8_t value;

    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void helper_serial_init(void)
{
    helper_outb(0x3F9U, 0x00U);
    helper_outb(0x3FBU, 0x80U);
    helper_outb(0x3F8U, 0x01U);
    helper_outb(0x3F9U, 0x00U);
    helper_outb(0x3FBU, 0x03U);
    helper_outb(0x3FAU, 0xC7U);
    helper_outb(0x3FCU, 0x0BU);
}

static void helper_serial_write(const char *text)
{
    size_t index;

    if (text == NULL) {
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        while ((helper_inb(0x3FDU) & 0x20U) == 0U) {
        }
        helper_outb(0x3F8U, (uint8_t)text[index]);
    }
}

static void helper_serial_write_u64(uint64_t value)
{
    char digits[21U];
    size_t length = 0U;

    do {
        digits[length++] =
            (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (length != 0U) {
        char character = digits[--length];

        helper_serial_write((char[2U]){ character, '\0' });
    }
}

static bool helper_device_path_length(
    const struct efi_device_path_protocol *path,
    size_t *length)
{
    size_t offset = 0U;

    if (path == NULL || length == NULL) {
        return false;
    }
    while (offset + sizeof(*path) <= EFI_BOOT_DEVICE_PATH_MAX) {
        const struct efi_device_path_protocol *node =
            (const struct efi_device_path_protocol *)
                ((const unsigned char *)path + offset);

        if (node->length < sizeof(*node) ||
            node->length > EFI_BOOT_DEVICE_PATH_MAX - offset) {
            return false;
        }
        offset += node->length;
        if (node->type == 0x7FU &&
            node->sub_type == 0xFFU) {
            *length = offset;
            return true;
        }
    }
    return false;
}

static enum block_device_status helper_block_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct hfoc_efi_block_context *context =
        (struct hfoc_efi_block_context *)device->context;
    const uint64_t lba = offset / device->block_size;
    if (context == NULL || context->block_io == NULL ||
        context->block_io->media == NULL ||
        context->block_io->read_blocks == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (context->block_io->read_blocks(
            context->block_io,
            context->block_io->media->media_id,
            lba, length, buffer) != EFI_SUCCESS) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status helper_block_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct hfoc_efi_block_context *context =
        (struct hfoc_efi_block_context *)device->context;
    const uint64_t lba = offset / device->block_size;

    if (context == NULL || context->block_io == NULL ||
        context->block_io->media == NULL ||
        context->block_io->write_blocks == NULL ||
        context->block_io->media->read_only) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (context->block_io->write_blocks(
            context->block_io,
            context->block_io->media->media_id,
            lba, length, buffer) != EFI_SUCCESS) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status helper_block_flush(
    struct block_device *device)
{
    struct hfoc_efi_block_context *context =
        (struct hfoc_efi_block_context *)device->context;

    if (context == NULL || context->block_io == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    if (context->block_io->flush_blocks == NULL ||
        context->block_io->flush_blocks(context->block_io) ==
            EFI_SUCCESS) {
        return BLOCK_DEVICE_OK;
    }
    return BLOCK_DEVICE_IO_ERROR;
}

static size_t helper_enumerate_block_devices(
    efi_handle **handles,
    size_t *handle_count)
{
    efi_handle *found = NULL;
    size_t found_count = 0U;
    size_t candidate_count = 0U;
    size_t index;

    if (system_table == NULL ||
        system_table->boot_services == NULL ||
        handles == NULL || handle_count == NULL ||
        system_table->boot_services->locate_handle_buffer == NULL ||
        system_table->boot_services->free_pool == NULL) {
        return 0U;
    }
    if (system_table->boot_services->locate_handle_buffer(
            EFI_BY_PROTOCOL,
            (struct efi_guid *)&efi_block_io_guid,
            NULL, &found_count, &found) != EFI_SUCCESS ||
        found == NULL) {
        return 0U;
    }
    if (found_count > HFOC_EFI_HELPER_MAX_BLOCK_HANDLES) {
        (void)system_table->boot_services->free_pool(found);
        return 0U;
    }
    for (index = 0U; index < found_count; ++index) {
        struct efi_block_io_protocol *block_io = NULL;
        struct efi_device_path_protocol *device_path = NULL;
        struct hfoc_efi_block_context *context =
            &block_contexts[candidate_count];
        size_t path_length = 0U;

        if (system_table->boot_services->handle_protocol(
                found[index], (struct efi_guid *)&efi_block_io_guid,
                (void **)&block_io) != EFI_SUCCESS ||
            block_io == NULL || block_io->media == NULL ||
            !block_io->media->media_present ||
            block_io->media->logical_partition ||
            block_io->media->read_only ||
            block_io->media->block_size != 512U ||
            block_io->media->last_block == UINT64_MAX ||
            block_io->media->last_block + 1U == 0U ||
            block_io->media->last_block + 1U >
                BLOCK_DEVICE_BLOCK_COUNT_MAX ||
            system_table->boot_services->handle_protocol(
                found[index], (struct efi_guid *)&efi_device_path_guid,
                (void **)&device_path) != EFI_SUCCESS ||
            !helper_device_path_length(
                device_path, &path_length)) {
            continue;
        }
        memset(context, 0, sizeof(*context));
        context->block_io = block_io;
        context->device_path = device_path;
        context->device_path_length = path_length;
        memset(&block_devices[candidate_count], 0,
               sizeof(block_devices[candidate_count]));
        block_devices[candidate_count].block_size =
            block_io->media->block_size;
        block_devices[candidate_count].block_count =
            block_io->media->last_block + 1U;
        block_devices[candidate_count].context = context;
        block_devices[candidate_count].read_fn = helper_block_read;
        block_devices[candidate_count].write_fn = helper_block_write;
        block_devices[candidate_count].flush_fn = helper_block_flush;
        ++candidate_count;
    }
    *handles = found;
    *handle_count = found_count;
    return candidate_count;
}

static bool helper_device_path_equals(
    const struct efi_device_path_protocol *left,
    size_t left_length,
    const struct efi_device_path_protocol *right,
    size_t right_length)
{
    return left_length == right_length &&
        memcmp(left, right, left_length) == 0;
}

static const struct efi_device_path_protocol *
helper_find_harddrive_node(
    const struct efi_device_path_protocol *path,
    size_t path_length)
{
    size_t offset = 0U;

    while (offset + sizeof(*path) <= path_length) {
        const struct efi_device_path_protocol *node =
            (const struct efi_device_path_protocol *)
                ((const unsigned char *)path + offset);

        if (node->length < sizeof(*node) ||
            node->length > path_length - offset) {
            return NULL;
        }
        if (node->type == 0x04U && node->sub_type == 0x01U &&
            node->length >= 42U) {
            return node;
        }
        offset += node->length;
    }
    return NULL;
}

static bool helper_find_boot_device_path(
    efi_handle *handles,
    size_t handle_count,
    const struct hfoc_efi_block_context *target_context,
    const struct hfoc_efi_installer_target *target,
    const unsigned char **path,
    size_t *path_length)
{
    const struct efi_device_path_protocol *found_path = NULL;
    size_t found_length = 0U;
    size_t found_count = 0U;
    size_t index;

    if (handles == NULL || target_context == NULL ||
        target == NULL || path == NULL || path_length == NULL) {
        return false;
    }
    for (index = 0U; index < handle_count; ++index) {
        struct efi_block_io_protocol *block_io = NULL;
        struct efi_device_path_protocol *device_path = NULL;
        const struct efi_device_path_protocol *harddrive;
        size_t device_path_length = 0U;
        size_t node_offset;
        uint64_t partition_start;
        uint64_t partition_size;

        if (system_table->boot_services->handle_protocol(
                handles[index],
                (struct efi_guid *)&efi_block_io_guid,
                (void **)&block_io) != EFI_SUCCESS ||
            block_io == NULL || block_io->media == NULL ||
            !block_io->media->media_present ||
            !block_io->media->logical_partition ||
            block_io->media->block_size != 512U ||
            system_table->boot_services->handle_protocol(
                handles[index],
                (struct efi_guid *)&efi_device_path_guid,
                (void **)&device_path) != EFI_SUCCESS ||
            !helper_device_path_length(
                device_path, &device_path_length)) {
            continue;
        }
        harddrive = helper_find_harddrive_node(
            device_path, device_path_length);
        if (harddrive == NULL) {
            continue;
        }
        node_offset = (size_t)(
            (const unsigned char *)harddrive -
            (const unsigned char *)device_path);
        if (node_offset + 4U !=
                target_context->device_path_length ||
            !helper_device_path_equals(
                target_context->device_path, node_offset,
                device_path, node_offset)) {
            continue;
        }
        memcpy(&partition_start,
               (const unsigned char *)harddrive + 8U,
               sizeof(partition_start));
        memcpy(&partition_size,
               (const unsigned char *)harddrive + 16U,
               sizeof(partition_size));
        if (partition_start != target->esp_first_lba ||
            partition_size != target->esp_block_count) {
            continue;
        }
        found_path = device_path;
        found_length = device_path_length;
        ++found_count;
    }
    if (found_count != 1U || found_path == NULL) {
        return false;
    }
    *path = (const unsigned char *)found_path;
    *path_length = found_length;
    return true;
}

static efi_char16 *helper_ascii_to_utf16(
    efi_char16 destination[EFI_BOOT_VARIABLE_NAME_MAX],
    const char *source)
{
    size_t index;

    if (destination == NULL || source == NULL) {
        return NULL;
    }
    for (index = 0U; index + 1U < EFI_BOOT_VARIABLE_NAME_MAX;
         ++index) {
        destination[index] = (efi_char16)(unsigned char)source[index];
        if (source[index] == '\0') {
            return destination;
        }
    }
    destination[EFI_BOOT_VARIABLE_NAME_MAX - 1U] = 0U;
    return source[EFI_BOOT_VARIABLE_NAME_MAX - 1U] == '\0'
        ? destination : NULL;
}

static enum efi_boot_variable_io_status helper_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    struct efi_guid guid;
    efi_char16 name[EFI_BOOT_VARIABLE_NAME_MAX];
    efi_status status;
    size_t data_size;

    (void)context;
    if (key == NULL || data == NULL || length == NULL ||
        helper_ascii_to_utf16(name, key->name) == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    memcpy(&guid, key->guid, sizeof(guid));
    data_size = capacity;
    status = system_table->runtime_services->get_variable(
        name, &guid, NULL, &data_size, data);
    if (status == EFI_SUCCESS) {
        *length = data_size;
        return data_size <= capacity
            ? EFI_BOOT_VARIABLE_IO_OK
            : EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
    }
    if (status == EFI_NOT_FOUND) {
        return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
    }
    if (status == EFI_BUFFER_TOO_SMALL) {
        return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
    }
    return EFI_BOOT_VARIABLE_IO_ERROR;
}

static enum efi_boot_variable_io_status helper_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    struct efi_guid guid;
    efi_char16 name[EFI_BOOT_VARIABLE_NAME_MAX];

    (void)context;
    if (key == NULL || data == NULL ||
        helper_ascii_to_utf16(name, key->name) == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    memcpy(&guid, key->guid, sizeof(guid));
    return system_table->runtime_services->set_variable(
               name, &guid, HFOC_EFI_HELPER_VARIABLE_ATTRIBUTES,
               length, data) == EFI_SUCCESS
        ? EFI_BOOT_VARIABLE_IO_OK
        : EFI_BOOT_VARIABLE_IO_ERROR;
}

static bool helper_read_payload_file(
    efi_handle volume_handle,
    const unsigned char **data,
    size_t *data_size)
{
    static const efi_char16 path[] =
        u"\\EFI\\HFOC\\PAYLOAD.EFI";
    unsigned char file_info[256U];
    struct efi_simple_file_system_protocol *file_system = NULL;
    struct efi_file_protocol *root = NULL;
    struct efi_file_protocol *file = NULL;
    size_t info_size = sizeof(file_info);
    uint64_t file_size;
    size_t read_size;
    bool success = false;

    if (volume_handle == NULL || data == NULL || data_size == NULL) {
        return false;
    }
    if (system_table->boot_services->handle_protocol(
            volume_handle,
            (struct efi_guid *)&efi_simple_file_system_guid,
            (void **)&file_system) != EFI_SUCCESS ||
        file_system == NULL || file_system->open_volume == NULL) {
        return false;
    }
    if (file_system->open_volume(file_system, (void **)&root) !=
            EFI_SUCCESS ||
        root == NULL || root->open == NULL || root->close == NULL) {
        goto cleanup;
    }
    if (root->open(root, &file, (efi_char16 *)path,
                   EFI_FILE_MODE_READ, 0U) != EFI_SUCCESS ||
        file == NULL || file->close == NULL ||
        file->get_info == NULL) {
        goto cleanup;
    }
    if (file->get_info(file, (struct efi_guid *)&efi_file_info_guid,
                       &info_size, file_info) != EFI_SUCCESS) {
        goto cleanup;
    }
    if (info_size < 16U) {
        goto cleanup;
    }
    if (file->set_position == NULL ||
        file->set_position(file, 0U) != EFI_SUCCESS ||
        file->read == NULL) {
        goto cleanup;
    }
    memcpy(&file_size, file_info + 8U, sizeof(file_size));
    if (file_size == 0U || file_size > sizeof(payload)) {
        goto cleanup;
    }
    read_size = (size_t)file_size;
    if (file->read(file, &read_size, payload) != EFI_SUCCESS ||
        read_size != file_size) {
        goto cleanup;
    }
    *data = payload;
    *data_size = read_size;
    success = true;

cleanup:
    if (file != NULL && file->close != NULL) {
        (void)file->close(file);
    }
    if (root != NULL && root->close != NULL) {
        (void)root->close(root);
    }
    return success;
}

static bool helper_load_payload(
    const unsigned char **data,
    size_t *data_size)
{
    efi_handle *handles = NULL;
    size_t handle_count = 0U;
    size_t found_count = 0U;
    size_t index;

    if (data == NULL || data_size == NULL ||
        system_table == NULL ||
        system_table->boot_services == NULL ||
        system_table->boot_services->locate_handle_buffer == NULL ||
        system_table->boot_services->free_pool == NULL ||
        system_table->boot_services->locate_handle_buffer(
            EFI_BY_PROTOCOL,
            (struct efi_guid *)&efi_simple_file_system_guid,
            NULL, &handle_count, &handles) != EFI_SUCCESS ||
        handles == NULL || handle_count == 0U) {
        return false;
    }
    if (handle_count > HFOC_EFI_HELPER_MAX_VOLUME_HANDLES) {
        (void)system_table->boot_services->free_pool(handles);
        return false;
    }
    for (index = 0U; index < handle_count; ++index) {
        const unsigned char *candidate = NULL;
        size_t candidate_size = 0U;

        if (!helper_read_payload_file(
                handles[index], &candidate, &candidate_size)) {
            continue;
        }
        if (candidate == NULL || candidate_size == 0U ||
            ++found_count != 1U) {
            (void)system_table->boot_services->free_pool(handles);
            return false;
        }
        *data = candidate;
        *data_size = candidate_size;
    }
    (void)system_table->boot_services->free_pool(handles);
    return found_count == 1U;
}

static efi_status helper_exit(
    efi_handle image_handle,
    enum hfoc_efi_installer_status status)
{
    const efi_status exit_status =
        status == HFOC_EFI_INSTALLER_OK
            ? EFI_SUCCESS
            : EFI_ABORTED;

    if (system_table != NULL &&
        system_table->boot_services != NULL &&
        system_table->boot_services->exit != NULL) {
        return system_table->boot_services->exit(
            image_handle, exit_status, 0U, NULL);
    }
    return exit_status;
}

efi_status EFIAPI efi_main(
    efi_handle image_handle,
    struct efi_system_table *table)
{
    struct hfoc_efi_installer_target target;
    struct efi_boot_environment environment;
    efi_handle *handles = NULL;
    size_t handle_count = 0U;
    size_t device_count;
    size_t payload_size = 0U;
    const unsigned char *payload_data = NULL;
    const unsigned char *boot_device_path = NULL;
    size_t boot_device_path_length = 0U;
    enum hfoc_efi_installer_status status;
    enum efi_boot_environment_status environment_status;
    enum efi_boot_environment_status path_status;
    struct hfoc_efi_block_context *target_context;

    system_table = table;
    helper_serial_init();
    helper_serial_write("[HFOC-EFI] begin\n");
    if (system_table == NULL ||
        system_table->runtime_services == NULL ||
        system_table->boot_services == NULL) {
        helper_serial_write("[HFOC-EFI] status=INVALID_ENVIRONMENT\n");
        return EFI_LOAD_ERROR;
    }

    device_count = helper_enumerate_block_devices(
        &handles, &handle_count);
    if (device_count == 0U) {
        helper_serial_write("[HFOC-EFI] status=NO_BLOCK_DEVICES\n");
        return helper_exit(image_handle, HFOC_EFI_INSTALLER_NO_TARGET);
    }
    memset(&target, 0, sizeof(target));
    status = hfoc_efi_installer_select_target(
        block_devices, device_count, &target);
    helper_serial_write("[HFOC-EFI] selection=");
    helper_serial_write(hfoc_efi_installer_status_string(status));
    helper_serial_write(" discovered=");
    helper_serial_write_u64(device_count);
    helper_serial_write("\n");
    if (status != HFOC_EFI_INSTALLER_OK) {
        return helper_exit(image_handle, status);
    }
    target_context =
        (struct hfoc_efi_block_context *)target.device->context;
    if (!helper_find_boot_device_path(
            handles, handle_count, target_context, &target,
            &boot_device_path, &boot_device_path_length)) {
        helper_serial_write("[HFOC-EFI] status=BOOT_PATH_UNAVAILABLE\n");
        return helper_exit(
            image_handle, HFOC_EFI_INSTALLER_EFI_ENVIRONMENT);
    }
    if (!helper_load_payload(&payload_data, &payload_size)) {
        helper_serial_write("[HFOC-EFI] status=PAYLOAD_UNAVAILABLE\n");
        return helper_exit(
            image_handle, HFOC_EFI_INSTALLER_INVALID_PAYLOAD);
    }
    memset(&environment, 0, sizeof(environment));
    environment_status = efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI,
            target.device, NULL, helper_variable_read,
            helper_variable_write);
    path_status = environment_status == EFI_BOOT_ENVIRONMENT_OK
        ? efi_boot_environment_set_device_path(
            &environment, boot_device_path,
            boot_device_path_length)
        : environment_status;
    if (environment_status != EFI_BOOT_ENVIRONMENT_OK ||
        path_status != EFI_BOOT_ENVIRONMENT_OK) {
        helper_serial_write("[HFOC-EFI] environment=");
        helper_serial_write_u64(environment_status);
        helper_serial_write(" boot_path=");
        helper_serial_write_u64(path_status);
        helper_serial_write(" path_length=");
        helper_serial_write_u64(boot_device_path_length);
        helper_serial_write("\n");
        return helper_exit(
            image_handle, HFOC_EFI_INSTALLER_EFI_ENVIRONMENT);
    }
    status = hfoc_efi_installer_apply(
        &environment, &target, payload_data, payload_size);
    helper_serial_write("[HFOC-EFI] status=");
    helper_serial_write(hfoc_efi_installer_status_string(status));
    if (status == HFOC_EFI_INSTALLER_OK) {
        struct efi_boot_variables_snapshot snapshot;

        if (efi_boot_variables_snapshot(
                &environment, &snapshot) ==
            EFI_BOOT_VARIABLES_OK) {
            helper_serial_write(" boot_order_entries=");
            helper_serial_write_u64(
                snapshot.boot_order_length / 2U);
            helper_serial_write(" boot_entries=");
            helper_serial_write_u64(snapshot.entry_count);
        }
    }
    helper_serial_write(" esp_lba=");
    helper_serial_write_u64(target.esp_first_lba);
    helper_serial_write(" payload_bytes=");
    helper_serial_write_u64(payload_size);
    helper_serial_write("\n");
    return helper_exit(image_handle, status);
}
