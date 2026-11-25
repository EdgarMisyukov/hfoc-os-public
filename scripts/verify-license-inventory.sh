#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
# Strict verifier for the ITEM-382 declared dependency inventory.
#
# `release/hfoc-license-inventory.txt` is the single source of truth for the
# panel about/licenses page, the payload notices contract
# (`scripts/verify-license-notices.sh`) and the release SBOM dependency rows
# (`scripts/emit-license-sbom-rows.sh`). It is one flat key=value file so every
# consumer parses exactly the same bytes, and this verifier rejects a file that
# is structurally wrong, that declares a notice the repository does not carry,
# or that states the mbedTLS election as anything but the elected Apache-2.0.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
INVENTORY=${1:-$ROOT/release/hfoc-license-inventory.txt}
APPLIANCE_LICENSE='AGPL-3.0-only OR LicenseRef-HFOC-Commercial'

fail() { echo "License inventory: $*" >&2; exit 1; }

[[ -f "$INVENTORY" && ! -L "$INVENTORY" ]] || fail "missing inventory: $INVENTORY"

value() {
    local key=$1
    local found

    [[ $(grep -c "^${key}=" "$INVENTORY") -eq 1 ]] ||
        fail "inventory needs exactly one ${key}"
    found=$(sed -n "s/^${key}=//p" "$INVENTORY")
    [[ -n "$found" ]] || fail "inventory has empty ${key}"
    printf '%s' "$found"
}

optional() {
    local key=$1
    local found

    [[ $(grep -c "^${key}=" "$INVENTORY") -le 1 ]] ||
        fail "inventory repeats ${key}"
    found=$(sed -n "s/^${key}=//p" "$INVENTORY")
    printf '%s' "$found"
}

# Every line is either a comment, blank, or one known key. An unknown key is a
# typo that would otherwise silently stop driving a consumer.
while IFS= read -r line; do
    [[ -z "$line" || "$line" == \#* ]] && continue
    [[ "$line" =~ ^[a-z][a-z0-9_]*=[^[:space:]] ]] ||
        fail "invalid inventory line: $line"
    key=${line%%=*}
    case "$key" in
        hfoc_license_inventory_version|appliance_component|appliance_license|\
        appliance_license_notice|appliance_licensing_terms|\
        appliance_commercial_terms|payload_manifest|payload_root|\
        dependency_count)
            ;;
        dependency_[0-9]*_id|dependency_[0-9]*_component|\
        dependency_[0-9]*_version|dependency_[0-9]*_license|\
        dependency_[0-9]*_license_offered|dependency_[0-9]*_election|\
        dependency_[0-9]*_notice|dependency_[0-9]*_election_notice)
            [[ "$key" =~ ^dependency_([1-9][0-9]*)_ ]] ||
                fail "invalid dependency index: $key"
            ;;
        *)
            fail "unsupported inventory key: $key"
            ;;
    esac
done <"$INVENTORY"

relative_path() {
    local label=$1
    local path=$2

    [[ -n "$path" ]] || fail "empty ${label}"
    [[ "$path" != /* ]] || fail "${label} is absolute: $path"
    [[ "$path" != *..* ]] || fail "${label} escapes the payload: $path"
    [[ "$path" != *\\* ]] || fail "${label} contains a backslash: $path"
    [[ "$path" != *'//'* ]] || fail "${label} contains a duplicate slash: $path"
    [[ "$path" != */ ]] || fail "${label} is a directory: $path"
}

# Map a payload-relative path to the repository file that ships it. The two
# renamed license texts are the only entries that do not mirror their source.
repository_source() {
    local path=$1

    case "$path" in
        AGPL-3.0-only.txt) printf '%s' 'LICENSE' ;;
        MIT-legacy.txt) printf '%s' 'LICENSES/MIT-legacy.txt' ;;
        *) printf '%s' "$path" ;;
    esac
}

require_repository_file() {
    local label=$1
    local path=$2
    local source

    relative_path "$label" "$path"
    source=$(repository_source "$path")
    [[ -f "$ROOT/$source" && ! -L "$ROOT/$source" ]] ||
        fail "${label} is not shipped by the repository: $source"
}

version=$(value hfoc_license_inventory_version)
[[ "$version" == "1" ]] || fail "unsupported inventory version: $version"

appliance_component=$(value appliance_component)
[[ -n "$appliance_component" ]] || fail 'appliance component is empty'
[[ "$appliance_component" == "HFOC Appliance OS" ]] ||
    fail "unexpected appliance component: $appliance_component"
appliance_license=$(value appliance_license)
[[ "$appliance_license" == "$APPLIANCE_LICENSE" ]] ||
    fail "appliance license is not the canonical dual license: $appliance_license"
appliance_license_notice=$(value appliance_license_notice)
appliance_licensing_terms=$(value appliance_licensing_terms)
appliance_commercial_terms=$(value appliance_commercial_terms)
payload_manifest=$(value payload_manifest)
payload_root=$(value payload_root)
dependency_count=$(value dependency_count)

[[ "$payload_root" =~ ^[a-z0-9][a-z0-9._-]*$ ]] ||
    fail "invalid payload root: $payload_root"
[[ "$payload_manifest" != */* ]] ||
    fail "payload manifest must live at the payload root: $payload_manifest"
require_repository_file 'appliance license notice' "$appliance_license_notice"
require_repository_file 'appliance licensing terms' "$appliance_licensing_terms"
require_repository_file 'appliance commercial terms' "$appliance_commercial_terms"

[[ "$dependency_count" =~ ^[1-9][0-9]*$ ]] ||
    fail "invalid dependency count: $dependency_count"

declare -A seen_ids=()
election_seen=0
mbedtls_seen=0

for ((index = 1; index <= dependency_count; ++index)); do
    prefix="dependency_${index}_"
    id=$(value "${prefix}id")
    component=$(value "${prefix}component")
    dependency_version=$(value "${prefix}version")
    license=$(value "${prefix}license")
    offered=$(optional "${prefix}license_offered")
    election=$(optional "${prefix}election")
    notice=$(value "${prefix}notice")
    election_notice=$(optional "${prefix}election_notice")

    [[ "$id" =~ ^[a-z0-9][a-z0-9-]*$ ]] || fail "invalid dependency id: $id"
    [[ -z ${seen_ids[$id]:-} ]] || fail "duplicate dependency id: $id"
    seen_ids[$id]=1
    [[ "$license" =~ ^[A-Za-z0-9][A-Za-z0-9.+-]*$ ]] ||
        fail "dependency ${id} license is not a single SPDX identifier: $license"
    [[ -n "$component" && "$component" != *'|'* && "$component" != *'<'* ]] ||
        fail "dependency ${id} has an unusable component name: $component"
    require_repository_file "dependency ${id} notice" "$notice"

    if [[ -n "$offered" ]]; then
        [[ "$offered" =~ ^[A-Za-z0-9][A-Za-z0-9.+-]*(\ OR\ [A-Za-z0-9][A-Za-z0-9.+-]*)+$ ]] ||
            fail "dependency ${id} offered license is not a disjunction: $offered"
        [[ -n "$election" ]] ||
            fail "dependency ${id} offers ${offered} without recording an election"
        [[ "$election" == "$license" ]] ||
            fail "dependency ${id} elects ${election} but ships ${license}"
        alternative_seen=0
        while IFS= read -r alternative; do
            [[ "$alternative" == "$election" ]] && alternative_seen=1
        done < <(printf '%s\n' "$offered" | sed 's/ OR /\n/g')
        (( alternative_seen == 1 )) ||
            fail "dependency ${id} elects ${election}, which ${offered} does not offer"
        [[ -n "$election_notice" ]] ||
            fail "dependency ${id} records an election without an election notice"
        require_repository_file \
            "dependency ${id} election notice" "$election_notice"
        election_seen=1
    else
        [[ -z "$election" ]] ||
            fail "dependency ${id} records an election without an offered disjunction"
        [[ -z "$election_notice" ]] ||
            fail "dependency ${id} ships an election notice without an election"
    fi

    if [[ "$id" == "mbedtls" ]]; then
        mbedtls_seen=1
        [[ "$license" == "Apache-2.0" ]] ||
            fail "mbedTLS election must be stated as Apache-2.0, not ${license}"
        [[ "$offered" == "Apache-2.0 OR GPL-2.0-or-later" ]] ||
            fail "mbedTLS offered license changed: ${offered:-unset}"
        [[ "$election" == "Apache-2.0" ]] ||
            fail "mbedTLS election must be Apache-2.0, not ${election:-unset}"
        [[ "$notice" == "third_party/mbedtls/LICENSE" ]] ||
            fail "mbedTLS notice moved: $notice"
        [[ "$election_notice" == "notices/mbedtls-Apache-2.0-election.txt" ]] ||
            fail "mbedTLS election notice moved: $election_notice"
    fi

    [[ -n "$dependency_version" ]] || fail "dependency ${id} has no version"
done

(( election_seen == 1 )) || fail 'no dependency records a license election'
(( mbedtls_seen == 1 )) || fail 'the mbedTLS dependency is not declared'

# No field may address a dependency the count does not declare.
while IFS= read -r line; do
    [[ "$line" =~ ^dependency_([0-9]+)_ ]] || continue
    (( BASH_REMATCH[1] >= 1 && BASH_REMATCH[1] <= dependency_count )) ||
        fail "inventory carries an undeclared dependency index: ${BASH_REMATCH[1]}"
done <"$INVENTORY"

echo "License inventory verification: OK"
