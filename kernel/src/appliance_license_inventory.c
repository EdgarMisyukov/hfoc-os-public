#include "appliance_license_inventory.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * The declared inventory. Every value here is mirrored from
 * `release/hfoc-license-inventory.txt` and is cross-checked against that file
 * by the host proof; the notice paths are payload-relative, and the payload
 * contract forces each of them onto the ISO.
 */
static const struct appliance_license_inventory_dependency
appliance_license_inventory_dependencies
    [APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT] = {
        {
            "lwip",
            "lwIP",
            "STABLE-2_2_1_RELEASE",
            "BSD-3-Clause",
            NULL,
            NULL,
            "third_party/lwip/COPYING",
            NULL,
        },
        {
            "mbedtls",
            "mbedTLS",
            "mbedtls-3.6.7",
            "Apache-2.0",
            "Apache-2.0 OR GPL-2.0-or-later",
            "Apache-2.0",
            "third_party/mbedtls/LICENSE",
            "notices/mbedtls-Apache-2.0-election.txt",
        },
        {
            "limine",
            "Limine",
            "12.3.2",
            "BSD-2-Clause",
            NULL,
            NULL,
            "third_party/limine/COPYING",
            NULL,
        },
        {
            "console-setup-font",
            "Debian console-setup Fixed 8x16 console font",
            "console-setup-linux CyrSlav-Fixed16.psf.gz (2025-10-19)",
            "LicenseRef-HFOC-Console-Fixed8x16",
            NULL,
            NULL,
            "notices/console-font-fixed8x16.txt",
            NULL,
        },
};

const char *appliance_license_inventory_component(void)
{
    return "HFOC Appliance OS";
}

const char *appliance_license_inventory_license(void)
{
    return "AGPL-3.0-only OR LicenseRef-HFOC-Commercial";
}

const char *appliance_license_inventory_license_notice(void)
{
    return "AGPL-3.0-only.txt";
}

const char *appliance_license_inventory_licensing_terms(void)
{
    return "LICENSING.md";
}

const char *appliance_license_inventory_commercial_terms(void)
{
    return "COMMERCIAL-LICENSING.md";
}

const char *appliance_license_inventory_manifest(void)
{
    return "LICENSES.sha256";
}

const char *appliance_license_inventory_payload_root(void)
{
    return "licenses";
}

uint32_t appliance_license_inventory_dependency_count(void)
{
    return APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT;
}

const struct appliance_license_inventory_dependency *
appliance_license_inventory_dependency(uint32_t index)
{
    if (index >= APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT) {
        return NULL;
    }
    return &appliance_license_inventory_dependencies[index];
}

static bool appliance_license_inventory_id_equal(
    const char *left,
    const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) {
            return false;
        }
        ++index;
    }
    return left[index] == right[index];
}

const struct appliance_license_inventory_dependency *
appliance_license_inventory_find(const char *id)
{
    uint32_t index;

    if (id == NULL) {
        return NULL;
    }
    for (index = 0U;
         index < APPLIANCE_LICENSE_INVENTORY_DEPENDENCY_COUNT;
         ++index) {
        if (appliance_license_inventory_id_equal(
                appliance_license_inventory_dependencies[index].id, id)) {
            return &appliance_license_inventory_dependencies[index];
        }
    }
    return NULL;
}

const char *appliance_license_inventory_mbedtls_election(void)
{
    const struct appliance_license_inventory_dependency *dependency =
        appliance_license_inventory_find("mbedtls");

    if (dependency == NULL || dependency->election == NULL) {
        return NULL;
    }
    return dependency->election;
}

const char *
appliance_license_inventory_notice(
    const struct appliance_license_inventory_dependency *dependency,
    uint32_t slot)
{
    if (dependency == NULL) {
        return NULL;
    }
    if (slot == 0U) {
        return dependency->notice;
    }
    if (slot == 1U) {
        return dependency->election_notice;
    }
    return NULL;
}
