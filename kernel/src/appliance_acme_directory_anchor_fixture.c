/*
 * ITEM-508 increments 15 and 17: the fixture trust anchor of the offline
 * ACME proofs.
 *
 * The production anchor is ISRG Root X1, an RSA certificate; the frozen
 * mbedTLS profile carries no RSA, so a proof that dials a fixture stub must
 * anchor a fixture chain.  This translation unit defines the same two symbols
 * `kernel/src/appliance_acme_trust_anchor.c` defines, from the increment-14
 * fixture CA, and is compiled only into the `ACME_DIRECTORY_*` and
 * `ACME_ORDER_*` kernel families (where the production object is filtered out
 * of the link).  Every other kernel build compiles an empty object.
 */
#if defined(HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST)

#include "appliance_acme_trust.h"
#include "appliance-acme-directory-fixtures.h"

const unsigned char *appliance_acme_trust_anchor_der_data(void)
{
    return directory_fixture_ca;
}

size_t appliance_acme_trust_anchor_der_length(void)
{
    return sizeof(directory_fixture_ca);
}

#endif /* HFOC_APPLIANCE_ACME_DIRECTORY_QEMU_TEST ||
          HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST ||
          HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST */

/*
 * Every kernel build compiles this wildcard source, and only the fixture
 * families define a test macro: the guard keeps the other translation units
 * non-empty and warning-free.
 */
typedef int appliance_acme_directory_anchor_fixture_translation_unit_guard;
