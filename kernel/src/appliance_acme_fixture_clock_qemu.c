/*
 * ITEM-508 increment 31: one fixture epoch for the ACME fixture families.
 *
 * The order proof (increment 17), the HTTP-01 wire proof (increment 29) and
 * the production order-path wire proof (increment 36) run without an SNTP
 * peer: the production wall clock is zero, and the decision module refuses a
 * zero clock by name instead of arming an answer that never expires.  The
 * families need the same nonzero instant, so the value is named exactly once,
 * here, and is not secret.  Every build outside those families compiles an
 * empty guarded object.
 */
#if defined(HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST)

#include "appliance_acme_fixture_clock_qemu.h"

#include <stdint.h>

/*
 * Nonzero and far from the wrap the decision module refuses; the 900 s answer
 * window runs from it.  A fixture that read its own clock could arm an answer
 * that is already over, which is why this is a constant and not a reading.
 */
#define APPLIANCE_ACME_FIXTURE_EPOCH UINT32_C(1800000000)

uint32_t appliance_acme_fixture_epoch_seconds(void)
{
    return APPLIANCE_ACME_FIXTURE_EPOCH;
}

#endif /* HFOC_APPLIANCE_ACME_HTTP01_QEMU_TEST ||
          HFOC_APPLIANCE_ACME_ORDER_QEMU_TEST ||
          HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST */

/*
 * Every kernel build compiles this wildcard source, and only the fixture
 * families define the macros above: the guard keeps the other translation
 * units non-empty and warning-free.
 */
typedef int appliance_acme_fixture_clock_qemu_translation_unit_guard;
