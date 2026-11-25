#include "production_installer_efi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum production_installer_status
production_efi_map_variables(enum efi_boot_variables_status status)
{
    switch (status) {
    case EFI_BOOT_VARIABLES_OK:
        return PRODUCTION_INSTALLER_OK;
    case EFI_BOOT_VARIABLES_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case EFI_BOOT_VARIABLES_INVALID_ENVIRONMENT:
    case EFI_BOOT_VARIABLES_INVALID_SNAPSHOT:
    case EFI_BOOT_VARIABLES_BAD_STATE:
    case EFI_BOOT_VARIABLES_NOT_CONFIRMED:
    case EFI_BOOT_VARIABLES_AMBIGUOUS:
        return PRODUCTION_INSTALLER_BAD_STATE;
    case EFI_BOOT_VARIABLES_VARIABLE_UNAVAILABLE:
        return PRODUCTION_INSTALLER_EFI_UNAVAILABLE;
    case EFI_BOOT_VARIABLES_VARIABLE_IO_ERROR:
    case EFI_BOOT_VARIABLES_MUTATION_FAILED:
        return PRODUCTION_INSTALLER_IO_ERROR;
    case EFI_BOOT_VARIABLES_CAPACITY:
        return PRODUCTION_INSTALLER_CAPACITY;
    case EFI_BOOT_VARIABLES_VERIFICATION_FAILED:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    default:
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
}

enum production_installer_status production_installer_efi_apply_boot(
    struct production_installer *installer,
    const struct efi_boot_environment *environment)
{
    /* Keep the 19 KiB EFI snapshot and plan out of the 16 KiB kernel stack. */
    static struct efi_boot_variables_snapshot snapshot;
    static struct efi_boot_variables_plan plan;
    enum efi_boot_variables_status status;

    if (installer == NULL || environment == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage !=
            PRODUCTION_INSTALLER_STAGE_EXECUTING ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (efi_boot_environment_validate(environment) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return PRODUCTION_INSTALLER_EFI_UNAVAILABLE;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    memset(&plan, 0, sizeof(plan));
    status = efi_boot_variables_snapshot(
        environment, &snapshot);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return production_efi_map_variables(status);
    }
    status = efi_boot_variables_plan(environment, &plan);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return production_efi_map_variables(status);
    }
    status = efi_boot_variables_confirm(&plan);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return production_efi_map_variables(status);
    }
    status = efi_boot_variables_apply(
        environment, &snapshot, &plan);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return production_efi_map_variables(status);
    }
    status = efi_boot_variables_verify(
        environment, &plan);
    if (status != EFI_BOOT_VARIABLES_OK) {
        return production_efi_map_variables(status);
    }
    return PRODUCTION_INSTALLER_OK;
}
