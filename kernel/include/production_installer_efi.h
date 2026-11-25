#ifndef HFOC_PRODUCTION_INSTALLER_EFI_H
#define HFOC_PRODUCTION_INSTALLER_EFI_H

#include "efi_boot_environment.h"
#include "efi_boot_variables.h"
#include "production_installer.h"

enum production_installer_status production_installer_efi_apply_boot(
    struct production_installer *installer,
    const struct efi_boot_environment *environment);

#endif
