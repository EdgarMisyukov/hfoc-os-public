#ifndef HFOC_APPLIANCE_IDENTITY_H
#define HFOC_APPLIANCE_IDENTITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_IDENTITY_VERSION 1U
#define APPLIANCE_IDENTITY_MACHINE_CLASS "hfoc-appliance"
#define APPLIANCE_IDENTITY_MACHINE_CLASS_LENGTH 14U
#define APPLIANCE_IDENTITY_PRODUCT "HFOC-APPLIANCE-V1"
#define APPLIANCE_IDENTITY_PRODUCT_LENGTH 17U
#define APPLIANCE_IDENTITY_ARCHITECTURE "x86_64"
#define APPLIANCE_IDENTITY_ARCHITECTURE_LENGTH 6U
#define APPLIANCE_IDENTITY_PLATFORM "q35"
#define APPLIANCE_IDENTITY_PLATFORM_LENGTH 3U
#define APPLIANCE_IDENTITY_DEFAULT_HOSTNAME "hfoc-appliance"
#define APPLIANCE_IDENTITY_DEFAULT_HOSTNAME_LENGTH 14U
#define APPLIANCE_IDENTITY_DEFAULT_INSTANCE "default"
#define APPLIANCE_IDENTITY_DEFAULT_INSTANCE_LENGTH 7U
#define APPLIANCE_IDENTITY_FIELD_MAX 24U
#define APPLIANCE_IDENTITY_RENDER_MAX 200U

enum appliance_identity_status {
    APPLIANCE_IDENTITY_OK = 0,
    APPLIANCE_IDENTITY_NULL = 1,
    APPLIANCE_IDENTITY_BAD_STATE = 2,
    APPLIANCE_IDENTITY_BAD_VERSION = 3,
    APPLIANCE_IDENTITY_BAD_FIELD = 4,
    APPLIANCE_IDENTITY_OVERFLOW = 5,
};

struct appliance_identity {
    uint32_t version;
    char machine_class[APPLIANCE_IDENTITY_FIELD_MAX];
    char product[APPLIANCE_IDENTITY_FIELD_MAX];
    char architecture[APPLIANCE_IDENTITY_FIELD_MAX];
    char platform[APPLIANCE_IDENTITY_FIELD_MAX];
    char hostname[APPLIANCE_IDENTITY_FIELD_MAX];
    char instance[APPLIANCE_IDENTITY_FIELD_MAX];
    bool published;
};

enum appliance_identity_status appliance_identity_boot_init(void);
const struct appliance_identity *appliance_identity_get(void);
enum appliance_identity_status appliance_identity_validate(
    const struct appliance_identity *identity);
enum appliance_identity_status appliance_identity_render(
    const struct appliance_identity *identity,
    char *output,
    size_t capacity,
    size_t *output_length);
const char *appliance_identity_status_string(
    enum appliance_identity_status status);

#endif
