#ifndef HFOC_APPLIANCE_LICENSE_INVENTORY_H
#define HFOC_APPLIANCE_LICENSE_INVENTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-382 declared dependency inventory.
 *
 * This is the in-kernel mirror of `release/hfoc-license-inventory.txt`, the
 * single declared source of truth. The mirror is not a second list that may
 * drift: `tests/appliance-license-inventory-host.c` reads the declared file and
 * fails when any component, version, license identifier, election or notice
 * path differs, and `scripts/check.sh` runs that proof together with
 * `scripts/verify-license-notices.sh`, which forces every declared notice into
 * the license payload that ships on the ISO.
 *
 * The module is allocation-free, holds only static storage, performs no I/O and
 * owns no policy: it is the data source the panel about/licenses page renders.
 */

#define APPLIANCE_LICENSE_INVENTORY_VERSION 1U
#define APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT 4U
/* A dependency declares at most a license notice and an election notice. */
#define APPLIANCE_LICENSE_INVENTORY_NOTICE_COUNT 2U

struct appliance_license_inventory_dependency {
    const char *id;
    const char *component;
    const char *version;
    /* The license the appliance ships this component under. */
    const char *license;
    /* The upstream expression when it is a disjunction, else NULL. */
    const char *license_offered;
    /* The elected branch of that disjunction, else NULL. */
    const char *election;
    /* Payload-relative license notice. */
    const char *notice;
    /* Payload-relative election notice, else NULL. */
    const char *election_notice;
};

const char *appliance_license_inventory_component(void);
/* The appliance's own dual license expression. */
const char *appliance_license_inventory_license(void);
const char *appliance_license_inventory_license_notice(void);
const char *appliance_license_inventory_licensing_terms(void);
const char *appliance_license_inventory_commercial_terms(void);
/* Payload-relative checksum manifest and the payload root it lives in. */
const char *appliance_license_inventory_manifest(void);
const char *appliance_license_inventory_payload_root(void);
uint32_t appliance_license_inventory_dependency_count(void);
const struct appliance_license_inventory_dependency *
appliance_license_inventory_dependency(uint32_t index);
const struct appliance_license_inventory_dependency *
appliance_license_inventory_find(const char *id);
/*
 * The license the mbedTLS component is elected under. It is always
 * "Apache-2.0": the appliance never ships mbedTLS under GPL-2.0-or-later and
 * never publishes the unresolved disjunction as its own license.
 */
const char *appliance_license_inventory_mbedtls_election(void);
/* Notice path slot for a dependency: 0 is the license notice, 1 the election
 * notice. Returns NULL when the slot does not exist. */
const char *
appliance_license_inventory_notice(
    const struct appliance_license_inventory_dependency *dependency,
    uint32_t slot);

#endif
