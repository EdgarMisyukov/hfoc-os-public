#ifndef HFOC_BOOT_CHAIN_BOOT_H
#define HFOC_BOOT_CHAIN_BOOT_H

#include "boot_attestation.h"
#include "boot_chain.h"
#include "boot_measurements.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-372 ordinary-boot composition.
 *
 * The boot environment hands over the sealed boot-chain manifest it loaded
 * and the exact bytes of the components the loader loaded.  This module
 * owns the production trust set (the declared release anchor, plus the
 * declared development anchor outside a release build), verifies the chain,
 * fills the measured-boot register set and publishes the attestation.  A
 * media without a signed manifest publishes UNAVAILABLE instead of claiming
 * a verified chain.
 */

struct boot_chain_boot_source {
    const void *manifest;
    size_t manifest_length;
    const struct boot_chain_component *components;
    size_t component_count;
};

enum boot_chain_status boot_chain_boot_run(
    const struct boot_chain_boot_source *source,
    struct boot_measurements *measurements,
    struct boot_attestation *attestation);

#endif
