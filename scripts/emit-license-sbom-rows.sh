#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
# ITEM-382 release SBOM dependency rows.
#
# Role: the release SBOM gate owned by ITEM-380 consumes these rows instead of
# keeping a second dependency list. The rows are derived only from
# `release/hfoc-license-inventory.txt`, so the panel page, the payload notices
# contract and the SBOM cannot drift apart. The output is deterministic (no
# timestamp, no locale, no network) and each block is an SPDX 2.3 tag:value
# package block with `FilesAnalyzed: false`.
#
# `PackageLicenseConcluded` is the license the appliance actually ships under,
# so the mbedTLS block concludes `Apache-2.0` and declares the offered
# `Apache-2.0 OR GPL-2.0-or-later`.
#
# Usage: emit-license-sbom-rows.sh [INVENTORY]
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
INVENTORY=${1:-${HFOC_LICENSE_INVENTORY:-$ROOT/release/hfoc-license-inventory.txt}}

fail() { echo "License SBOM rows: $*" >&2; exit 1; }
[[ -f "$INVENTORY" && ! -L "$INVENTORY" ]] || fail "missing inventory: $INVENTORY"

value() {
    local key=$1
    local found

    [[ $(grep -c "^${key}=" "$INVENTORY") -eq 1 ]] || fail "inventory needs ${key}"
    found=$(sed -n "s/^${key}=//p" "$INVENTORY")
    [[ -n "$found" ]] || fail "inventory has empty ${key}"
    printf '%s' "$found"
}

optional() {
    local key=$1

    sed -n "s/^${key}=//p" "$INVENTORY"
}

package_id() {
    printf 'SPDXRef-Package-%s' "$1"
}

appliance_component=$(value appliance_component)
appliance_license=$(value appliance_license)
appliance_license_notice=$(value appliance_license_notice)
appliance_commercial_terms=$(value appliance_commercial_terms)
payload_manifest=$(value payload_manifest)
dependency_count=$(value dependency_count)

printf '# HFOC license inventory SBOM rows (inventory version %s)\n' \
    "$(value hfoc_license_inventory_version)"
printf '# Source: release/hfoc-license-inventory.txt; consumed by the ITEM-380 release gate.\n'

printf 'SPDXID: %s\n' "$(package_id hfoc-appliance-os)"
printf 'PackageName: %s\n' "$appliance_component"
printf 'PackageVersion: %s\n' "$(cat "$ROOT/VERSION" 2>/dev/null || printf 'unversioned')"
printf 'PackageDownloadLocation: NOASSERTION\n'
printf 'FilesAnalyzed: false\n'
printf 'PackageLicenseConcluded: %s\n' "$appliance_license"
printf 'PackageLicenseDeclared: %s\n' "$appliance_license"
printf 'PackageLicenseComments: license payload notice %s, commercial terms %s, checksum manifest %s\n' \
    "$appliance_license_notice" "$appliance_commercial_terms" "$payload_manifest"
printf 'PackageCopyrightText: NOASSERTION\n'

for ((index = 1; index <= dependency_count; ++index)); do
    prefix="dependency_${index}_"
    id=$(value "${prefix}id")
    component=$(value "${prefix}component")
    dependency_version=$(value "${prefix}version")
    license=$(value "${prefix}license")
    notice=$(value "${prefix}notice")
    offered=$(optional "${prefix}license_offered")
    election=$(optional "${prefix}election")
    election_notice=$(optional "${prefix}election_notice")

    declared=$license
    [[ -n "$offered" ]] && declared=$offered
    printf '\n'
    printf 'SPDXID: %s\n' "$(package_id "$id")"
    printf 'PackageName: %s\n' "$component"
    printf 'PackageVersion: %s\n' "$dependency_version"
    printf 'PackageDownloadLocation: NOASSERTION\n'
    printf 'FilesAnalyzed: false\n'
    printf 'PackageLicenseConcluded: %s\n' "$license"
    printf 'PackageLicenseDeclared: %s\n' "$declared"
    if [[ -n "$election" ]]; then
        printf 'PackageLicenseComments: %s election recorded: ships as %s; upstream offers %s; notice %s; election notice %s\n' \
            "$component" "$election" "$offered" "$notice" "$election_notice"
    else
        printf 'PackageLicenseComments: notice shipped in the license payload as %s\n' \
            "$notice"
    fi
    printf 'PackageCopyrightText: NOASSERTION\n'
done
