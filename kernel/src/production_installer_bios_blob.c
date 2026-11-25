/*
 * ITEM-517 step 2: the pinned Limine BIOS blobs, installer kernel only.
 *
 * The legacy-BIOS boot path needs two artifacts of the pinned Limine 12.3.2
 * binary release and nothing else:
 *
 *   * the 22,516-byte stage image `binary_limine_hdd_bin_data` from
 *     `deps/limine/limine-bios-hdd.h` (stage1 = [0:512), stage2 =
 *     [512:22516));
 *   * `deps/limine/limine-bios.sys`, the 290,964-byte loader Limine's stage2
 *     reads from the installed boot volume.
 *
 * Both are carried by the installer kernel and by no product kernel.  The
 * header is included here, in the one translation unit that owns the array, so
 * a second inclusion cannot duplicate its definition; `limine-bios.sys` is
 * linked as the binary object the Makefile builds with
 * `$(LD) -r -b binary deps/limine/limine-bios.sys`.  The symbol names are the
 * ones `ld` derives from that exact path:
 *
 *   _binary_deps_limine_limine_bios_sys_start
 *   _binary_deps_limine_limine_bios_sys_end
 *   _binary_deps_limine_limine_bios_sys_size
 *
 * `production_installer_bios_boot.c` re-checks the sizes and SHA-256 digests
 * of the pinned release before it writes anything, so a partial cache or a
 * substituted blob is refused instead of installed.
 */

#include <stddef.h>
#include <stdint.h>

#include "production_installer_bios_boot.h"

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA

/* The linker symbols of the binary object (`ld -r -b binary`). */
extern const uint8_t _binary_deps_limine_limine_bios_sys_start[];
extern const uint8_t _binary_deps_limine_limine_bios_sys_end[];

/* Defines `binary_limine_hdd_bin_data[]` (22,516 bytes, no guard). */
#include "limine-bios-hdd.h"

struct production_installer_bios_blob production_installer_bios_blob(void)
{
    struct production_installer_bios_blob blob;

    blob.version = PRODUCTION_INSTALLER_BIOS_BOOT_VERSION;
    blob.stage = binary_limine_hdd_bin_data;
    blob.stage_size = sizeof(binary_limine_hdd_bin_data);
    blob.limine_bios_sys = _binary_deps_limine_limine_bios_sys_start;
    blob.limine_bios_sys_size =
        (size_t)(_binary_deps_limine_limine_bios_sys_end -
                 _binary_deps_limine_limine_bios_sys_start);
    return blob;
}

#else
struct production_installer_bios_blob production_installer_bios_blob(void)
{
    struct production_installer_bios_blob blob;

    blob.version = PRODUCTION_INSTALLER_BIOS_BOOT_VERSION;
    blob.stage = NULL;
    blob.stage_size = 0U;
    blob.limine_bios_sys = NULL;
    blob.limine_bios_sys_size = 0U;
    return blob;
}

/* Keep this production translation unit non-empty off the install medium. */
int hfoc_production_installer_bios_blob_translation_unit_marker;
#endif
