#include "appliance_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static struct appliance_identity runtime_identity;
static bool identity_initialized;

static bool identity_field_matches(
    const char *field,
    const char *expected,
    size_t length)
{
    return field[length] == '\0' &&
           memcmp(field, expected, length) == 0;
}

static enum appliance_identity_status appliance_identity_validate_impl(
    const struct appliance_identity *identity)
{
    if (identity == NULL) {
        return APPLIANCE_IDENTITY_NULL;
    }
    if (!identity->published) {
        return APPLIANCE_IDENTITY_BAD_STATE;
    }
    if (identity->version != APPLIANCE_IDENTITY_VERSION) {
        return APPLIANCE_IDENTITY_BAD_VERSION;
    }
    if (!identity_field_matches(
            identity->machine_class,
            APPLIANCE_IDENTITY_MACHINE_CLASS,
            APPLIANCE_IDENTITY_MACHINE_CLASS_LENGTH) ||
        !identity_field_matches(
            identity->product,
            APPLIANCE_IDENTITY_PRODUCT,
            APPLIANCE_IDENTITY_PRODUCT_LENGTH) ||
        !identity_field_matches(
            identity->architecture,
            APPLIANCE_IDENTITY_ARCHITECTURE,
            APPLIANCE_IDENTITY_ARCHITECTURE_LENGTH) ||
        !identity_field_matches(
            identity->platform,
            APPLIANCE_IDENTITY_PLATFORM,
            APPLIANCE_IDENTITY_PLATFORM_LENGTH) ||
        !identity_field_matches(
            identity->hostname,
            APPLIANCE_IDENTITY_DEFAULT_HOSTNAME,
            APPLIANCE_IDENTITY_DEFAULT_HOSTNAME_LENGTH) ||
        !identity_field_matches(
            identity->instance,
            APPLIANCE_IDENTITY_DEFAULT_INSTANCE,
            APPLIANCE_IDENTITY_DEFAULT_INSTANCE_LENGTH)) {
        return APPLIANCE_IDENTITY_BAD_FIELD;
    }
    return APPLIANCE_IDENTITY_OK;
}

static void identity_copy_literal(char *destination,
                                  const char *literal)
{
    size_t index;

    for (index = 0U; literal[index] != '\0'; ++index) {
        destination[index] = literal[index];
    }
    destination[index] = '\0';
}

enum appliance_identity_status appliance_identity_boot_init(void)
{
    if (identity_initialized) {
        return APPLIANCE_IDENTITY_BAD_STATE;
    }

    memset(&runtime_identity, 0, sizeof(runtime_identity));
    runtime_identity.version = APPLIANCE_IDENTITY_VERSION;
    identity_copy_literal(
        runtime_identity.machine_class,
        APPLIANCE_IDENTITY_MACHINE_CLASS);
    identity_copy_literal(
        runtime_identity.product,
        APPLIANCE_IDENTITY_PRODUCT);
    identity_copy_literal(
        runtime_identity.architecture,
        APPLIANCE_IDENTITY_ARCHITECTURE);
    identity_copy_literal(
        runtime_identity.platform,
        APPLIANCE_IDENTITY_PLATFORM);
    identity_copy_literal(
        runtime_identity.hostname,
        APPLIANCE_IDENTITY_DEFAULT_HOSTNAME);
    identity_copy_literal(
        runtime_identity.instance,
        APPLIANCE_IDENTITY_DEFAULT_INSTANCE);
    runtime_identity.published = true;
    identity_initialized = true;
    return APPLIANCE_IDENTITY_OK;
}

const struct appliance_identity *appliance_identity_get(void)
{
    if (!identity_initialized) {
        return NULL;
    }
    return &runtime_identity;
}

enum appliance_identity_status appliance_identity_validate(
    const struct appliance_identity *identity)
{
    return appliance_identity_validate_impl(identity);
}

enum appliance_identity_status appliance_identity_render(
    const struct appliance_identity *identity,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    static const char rendered[] =
        "identity v=1 class=hfoc-appliance product=HFOC-APPLIANCE-V1 "
        "arch=x86_64 platform=q35 hostname=hfoc-appliance "
        "instance=default\n";
    size_t length;
    size_t index;
    enum appliance_identity_status status;

    if (identity == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_IDENTITY_NULL;
    }
    status = appliance_identity_validate_impl(identity);
    if (status != APPLIANCE_IDENTITY_OK) {
        return status;
    }

    length = sizeof(rendered) - 1U;
    if (length > capacity) {
        return APPLIANCE_IDENTITY_OVERFLOW;
    }
    for (index = 0U; index < length; ++index) {
        output[index] = rendered[index];
    }
    output[length] = '\0';
    *output_length = length;
    return APPLIANCE_IDENTITY_OK;
}

const char *appliance_identity_status_string(
    enum appliance_identity_status status)
{
    switch (status) {
    case APPLIANCE_IDENTITY_OK:
        return "APPLIANCE_IDENTITY_OK";
    case APPLIANCE_IDENTITY_NULL:
        return "APPLIANCE_IDENTITY_NULL";
    case APPLIANCE_IDENTITY_BAD_STATE:
        return "APPLIANCE_IDENTITY_BAD_STATE";
    case APPLIANCE_IDENTITY_BAD_VERSION:
        return "APPLIANCE_IDENTITY_BAD_VERSION";
    case APPLIANCE_IDENTITY_BAD_FIELD:
        return "APPLIANCE_IDENTITY_BAD_FIELD";
    case APPLIANCE_IDENTITY_OVERFLOW:
        return "APPLIANCE_IDENTITY_OVERFLOW";
    default:
        return NULL;
    }
}
