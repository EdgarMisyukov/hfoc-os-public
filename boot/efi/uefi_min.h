#ifndef HFOC_UEFI_MIN_H
#define HFOC_UEFI_MIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EFIAPI __attribute__((ms_abi))

#define EFI_SUCCESS 0ULL
#define EFI_ERROR_MASK UINT64_C(0x8000000000000000)
#define EFI_LOAD_ERROR (EFI_ERROR_MASK | 1ULL)
#define EFI_INVALID_PARAMETER (EFI_ERROR_MASK | 2ULL)
#define EFI_BUFFER_TOO_SMALL (EFI_ERROR_MASK | 5ULL)
#define EFI_NOT_FOUND (EFI_ERROR_MASK | 14ULL)
#define EFI_ABORTED (EFI_ERROR_MASK | 21ULL)

#define EFI_ALL_HANDLES 0U
#define EFI_BY_PROTOCOL 2U
#define EFI_FILE_MODE_READ 0x0000000000000001ULL

typedef uint64_t efi_status;
typedef uint64_t efi_physical_address;
typedef uint64_t efi_virtual_address;
typedef uint64_t efi_tpl;
typedef void *efi_handle;
typedef uint16_t efi_char16;

struct efi_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t data4[8U];
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_simple_text_output_protocol;
struct efi_runtime_services;
struct efi_boot_services;
struct efi_device_path_protocol;

struct efi_system_table {
    struct efi_table_header header;
    efi_char16 *firmware_vendor;
    uint32_t firmware_revision;
    efi_handle console_in_handle;
    void *console_in;
    efi_handle console_out_handle;
    struct efi_simple_text_output_protocol *console_out;
    efi_handle standard_error_handle;
    struct efi_simple_text_output_protocol *standard_error;
    struct efi_runtime_services *runtime_services;
    struct efi_boot_services *boot_services;
};

typedef efi_status (EFIAPI *efi_get_variable_fn)(
    efi_char16 *variable_name,
    struct efi_guid *vendor_guid,
    uint32_t *attributes,
    size_t *data_size,
    void *data);
typedef efi_status (EFIAPI *efi_set_variable_fn)(
    efi_char16 *variable_name,
    struct efi_guid *vendor_guid,
    uint32_t attributes,
    size_t data_size,
    const void *data);

struct efi_runtime_services {
    struct efi_table_header header;
    void *get_time;
    void *set_time;
    void *get_wakeup_time;
    void *set_wakeup_time;
    void *set_virtual_address_map;
    void *convert_pointer;
    efi_get_variable_fn get_variable;
    void *get_next_variable_name;
    efi_set_variable_fn set_variable;
};

typedef efi_status (EFIAPI *efi_handle_protocol_fn)(
    efi_handle handle,
    struct efi_guid *protocol,
    void **interface);
typedef efi_status (EFIAPI *efi_locate_handle_buffer_fn)(
    uint32_t search_type,
    struct efi_guid *protocol,
    void *search_key,
    size_t *handle_count,
    efi_handle **buffer);
typedef efi_status (EFIAPI *efi_free_pool_fn)(void *buffer);
typedef efi_status (EFIAPI *efi_exit_fn)(
    efi_handle image_handle,
    efi_status exit_status,
    size_t exit_data_size,
    efi_char16 *exit_data);

struct efi_boot_services {
    struct efi_table_header header;
    void *raise_tpl;
    void *restore_tpl;
    void *allocate_pages;
    void *free_pages;
    void *get_memory_map;
    void *allocate_pool;
    efi_free_pool_fn free_pool;
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_handle_protocol_fn handle_protocol;
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    efi_exit_fn exit;
    void *unload_image;
    void *exit_boot_services;
    void *get_next_monotonic_count;
    void *stall;
    void *set_watchdog_timer;
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    efi_locate_handle_buffer_fn locate_handle_buffer;
};

struct efi_simple_text_output_protocol {
    void *reset;
    efi_status (EFIAPI *output_string)(
        struct efi_simple_text_output_protocol *this_protocol,
        efi_char16 *text);
    void *test_string;
    void *query_mode;
    void *set_mode;
    void *set_attribute;
    void *clear_screen;
    void *set_cursor_position;
    void *enable_cursor;
    void *mode;
};

struct efi_loaded_image_protocol {
    uint32_t revision;
    efi_handle parent_handle;
    struct efi_system_table *system_table;
    efi_handle device_handle;
    void *file_path;
    void *reserved;
    uint32_t load_options_size;
    void *load_options;
    void *image_base;
    uint64_t image_size;
    uint32_t image_code_type;
    uint32_t image_data_type;
    void *unload;
};

struct efi_device_path_protocol {
    uint8_t type;
    uint8_t sub_type;
    uint16_t length;
};

struct efi_block_io_media {
    uint32_t media_id;
    bool removable_media;
    bool media_present;
    bool logical_partition;
    bool read_only;
    bool write_caching;
    uint32_t block_size;
    uint32_t io_align;
    uint64_t last_block;
    uint64_t lowest_aligned_lba;
    uint32_t logical_blocks_per_physical_block;
    uint32_t optimal_transfer_length_granularity;
};

struct efi_block_io_protocol;

typedef efi_status (EFIAPI *efi_block_reset_fn)(
    struct efi_block_io_protocol *this_protocol,
    bool extended_verification);
typedef efi_status (EFIAPI *efi_block_read_fn)(
    struct efi_block_io_protocol *this_protocol,
    uint32_t media_id,
    uint64_t lba,
    size_t buffer_size,
    void *buffer);
typedef efi_status (EFIAPI *efi_block_write_fn)(
    struct efi_block_io_protocol *this_protocol,
    uint32_t media_id,
    uint64_t lba,
    size_t buffer_size,
    const void *buffer);
typedef efi_status (EFIAPI *efi_block_flush_fn)(
    struct efi_block_io_protocol *this_protocol);

struct efi_block_io_protocol {
    uint64_t revision;
    struct efi_block_io_media *media;
    efi_block_reset_fn reset;
    efi_block_read_fn read_blocks;
    efi_block_write_fn write_blocks;
    efi_block_flush_fn flush_blocks;
};

struct efi_simple_file_system_protocol;
typedef efi_status (EFIAPI *efi_open_volume_fn)(
    struct efi_simple_file_system_protocol *this_protocol,
    void **root);

struct efi_simple_file_system_protocol {
    uint64_t revision;
    efi_open_volume_fn open_volume;
};

struct efi_file_protocol;
typedef efi_status (EFIAPI *efi_file_open_fn)(
    struct efi_file_protocol *this_protocol,
    struct efi_file_protocol **new_handle,
    efi_char16 *file_name,
    uint64_t open_mode,
    uint64_t attributes);
typedef efi_status (EFIAPI *efi_file_close_fn)(
    struct efi_file_protocol *this_protocol);

struct efi_file_protocol {
    uint64_t revision;
    efi_file_open_fn open;
    efi_file_close_fn close;
    void *delete_file;
    efi_status (EFIAPI *read)(
        struct efi_file_protocol *this_protocol,
        size_t *buffer_size,
        void *buffer);
    void *write;
    void *get_position;
    efi_status (EFIAPI *set_position)(
        struct efi_file_protocol *this_protocol,
        uint64_t position);
    efi_status (EFIAPI *get_info)(
        struct efi_file_protocol *this_protocol,
        struct efi_guid *information_type,
        size_t *buffer_size,
        void *buffer);
};

#endif
