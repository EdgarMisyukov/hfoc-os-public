/*
 * Minimal subset of the Limine Boot Protocol header used by HFOC OS.
 * Protocol definitions are compatible with Limine base revision 6.
 * The original Limine protocol header is distributed under 0BSD.
 */
#ifndef HFOC_LIMINE_MIN_H
#define HFOC_LIMINE_MIN_H

#include <stdint.h>

#define LIMINE_REQUESTS_START_MARKER { \
    0xf6b8f4b39de7d1aeULL, 0xfab91a6940fcb9cfULL, \
    0x785c6ed015d3e316ULL, 0x181e920a7852b9d9ULL \
}
#define LIMINE_REQUESTS_END_MARKER { \
    0xadc0e0531bb10d03ULL, 0x9572709f31764c62ULL \
}
#define LIMINE_BASE_REVISION(n) { \
    0xf9562b2d5c95a6c8ULL, 0x6a7b384944536bdcULL, (n) \
}
#define LIMINE_BASE_REVISION_SUPPORTED(var) ((var)[2] == 0)
#define LIMINE_COMMON_MAGIC \
    0xc7b1dd30df4c8b88ULL, 0x0a82e883a194f07bULL

#define LIMINE_BOOTLOADER_INFO_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0xf55038d8e2a1202fULL, 0x279426fcf5f59740ULL \
}
#define LIMINE_HHDM_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0x48dcf1cb8ad2b852ULL, 0x63984e959a98244bULL \
}
#define LIMINE_FRAMEBUFFER_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0x9d5827dcd881dd75ULL, 0xa3148604f6fab11bULL \
}
#define LIMINE_MEMMAP_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0x67cf3d9d378a806fULL, 0xe304acdfc50c3c62ULL \
}
#define LIMINE_RSDP_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0xc5e77b6b397e7b43ULL, 0x27637845accdcf3cULL \
}
#define LIMINE_MP_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0x95a67b819a1b857eULL, 0xa0b61b723b6a73e0ULL \
}
#define LIMINE_EXECUTABLE_FILE_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0xad97e90e83f1ed67ULL, 0x31eb5d1c5ff23b69ULL \
}
#define LIMINE_MODULE_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0x3e7e279702be32afULL, 0xca1c4f3bd1280ceeULL \
}
/*
 * ITEM-394: the installer medium's clean install is a UEFI install (the
 * ESP role needs UEFI to boot the installed system), so the product flow
 * asks the loader which firmware booted it instead of assuming one.  The
 * request reports no response on a machine without EFI.
 */
#define LIMINE_FIRMWARE_TYPE_REQUEST_ID { \
    LIMINE_COMMON_MAGIC, 0x8c2f75d90bef28a8ULL, 0x7045a4688eac00c3ULL \
}
#define LIMINE_FIRMWARE_TYPE_X86BIOS 0U
#define LIMINE_FIRMWARE_TYPE_EFI32 1U
#define LIMINE_FIRMWARE_TYPE_EFI64 2U
#define LIMINE_FIRMWARE_TYPE_SBI 3U
#define LIMINE_MP_REQUEST_X86_64_X2APIC (1U << 0U)
#define LIMINE_MP_RESPONSE_X86_64_X2APIC (1U << 0U)

#define LIMINE_FRAMEBUFFER_RGB 1

#define LIMINE_MEMMAP_USABLE                 0
#define LIMINE_MEMMAP_RESERVED               1
#define LIMINE_MEMMAP_ACPI_RECLAIMABLE       2
#define LIMINE_MEMMAP_ACPI_NVS               3
#define LIMINE_MEMMAP_BAD_MEMORY             4
#define LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE 5
#define LIMINE_MEMMAP_EXECUTABLE_AND_MODULES 6
#define LIMINE_MEMMAP_FRAMEBUFFER            7
#define LIMINE_MEMMAP_RESERVED_MAPPED        8

struct limine_bootloader_info_response {
    uint64_t revision;
    char *name;
    char *version;
};

struct limine_bootloader_info_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_bootloader_info_response *response;
};

struct limine_hhdm_response {
    uint64_t revision;
    uint64_t offset;
};

struct limine_file {
    uint64_t revision;
    void *address;
    uint64_t size;
    char *path;
    char *string;
    uint32_t media_type;
    uint32_t unused;
    uint8_t tftp_ipv4[4];
    uint32_t tftp_port;
    uint32_t partition_index;
    uint32_t mbr_disk_id;
    struct {
        uint32_t a;
        uint16_t b;
        uint16_t c;
        uint8_t d[8];
    } gpt_disk_uuid;
    struct {
        uint32_t a;
        uint16_t b;
        uint16_t c;
        uint8_t d[8];
    } gpt_part_uuid;
    struct {
        uint32_t a;
        uint16_t b;
        uint16_t c;
        uint8_t d[8];
    } part_uuid;
};

struct limine_executable_file_response {
    uint64_t revision;
    struct limine_file *executable_file;
};

struct limine_executable_file_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_executable_file_response *response;
};

struct limine_module_response {
    uint64_t revision;
    uint64_t module_count;
    struct limine_file **modules;
};

struct limine_module_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_module_response *response;
    uint64_t internal_module_count;
    struct limine_internal_module **internal_modules;
};

struct limine_internal_module {
    const char *path;
    const char *string;
    uint64_t flags;
};

struct limine_firmware_type_response {
    uint64_t revision;
    uint64_t firmware_type;
};

struct limine_firmware_type_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_firmware_type_response *response;
};

struct limine_hhdm_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_hhdm_response *response;
};

struct limine_video_mode {
    uint64_t pitch;
    uint64_t width;
    uint64_t height;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
};

struct limine_framebuffer {
    void *address;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
    uint8_t unused[7];
    uint64_t edid_size;
    void *edid;
    uint64_t mode_count;
    struct limine_video_mode **modes;
};

struct limine_framebuffer_response {
    uint64_t revision;
    uint64_t framebuffer_count;
    struct limine_framebuffer **framebuffers;
};

struct limine_framebuffer_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_framebuffer_response *response;
};

struct limine_memmap_entry {
    uint64_t base;
    uint64_t length;
    uint64_t type;
};

struct limine_memmap_response {
    uint64_t revision;
    uint64_t entry_count;
    struct limine_memmap_entry **entries;
};

struct limine_memmap_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_memmap_response *response;
};

struct limine_rsdp_response {
    uint64_t revision;
    void *address;
};

struct limine_rsdp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_rsdp_response *response;
};

struct limine_mp_info {
    uint32_t processor_id;
    uint32_t lapic_id;
    uint64_t reserved;
    void (*goto_address)(struct limine_mp_info *);
    uint64_t extra_argument;
};

struct limine_mp_response {
    uint64_t revision;
    uint32_t flags;
    uint32_t bsp_lapic_id;
    uint64_t cpu_count;
    struct limine_mp_info **cpus;
};

struct limine_mp_request {
    uint64_t id[4];
    uint64_t revision;
    struct limine_mp_response *response;
    uint64_t flags;
};

#endif
