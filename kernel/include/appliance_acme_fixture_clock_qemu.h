#ifndef HFOC_APPLIANCE_ACME_FIXTURE_CLOCK_QEMU_H
#define HFOC_APPLIANCE_ACME_FIXTURE_CLOCK_QEMU_H

#include <stdint.h>

/*
 * ITEM-508 increment 31: the one wall clock both ACME fixture families run on.
 *
 * The proofs have no SNTP peer, so the production wall clock is zero, and the
 * decision module refuses a zero clock by name (`BAD_STATE` at arming,
 * `EXPIRED` on a request).  Each fixture family therefore names one nonzero
 * fixture epoch - the increment-17 order proof introduced the value, the
 * increment-29 wire proof needed it too - and this unit is where that value
 * lives once: the HTTP-01 family, the order family and the listener's `poll`
 * in `kernel.c` all read the same function.  A validated production clock is
 * ITEM-503's gate and is untouched by this unit, which every non-fixture build
 * compiles as an empty object.
 */
uint32_t appliance_acme_fixture_epoch_seconds(void);

#endif /* HFOC_APPLIANCE_ACME_FIXTURE_CLOCK_QEMU_H */
