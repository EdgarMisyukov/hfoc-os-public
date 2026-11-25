#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
# ITEM-382 payload notices contract.
#
# `release/hfoc-license-inventory.txt` declares every dependency the appliance
# ships. This verifier forces each declared dependency into the license payload
# that goes onto the ISO: the notice must be a regular file inside the payload,
# the payload checksum manifest must cover it with a matching digest, and the
# election notice must state the election the inventory records. Adding a
# dependency to the inventory without shipping its notice fails here and names
# the dependency.
#
# Usage: verify-license-notices.sh PAYLOAD_DIR [INVENTORY]
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
PAYLOAD=${1:-}
INVENTORY=${2:-${HFOC_LICENSE_INVENTORY:-$ROOT/release/hfoc-license-inventory.txt}}

fail() { echo "License notices: $*" >&2; exit 1; }

[[ -n "$PAYLOAD" ]] || {
    echo 'Usage: verify-license-notices.sh PAYLOAD_DIR [INVENTORY]' >&2
    exit 2
}
[[ -d "$PAYLOAD" && ! -L "$PAYLOAD" ]] || fail "missing payload: $PAYLOAD"
[[ -f "$INVENTORY" && ! -L "$INVENTORY" ]] || fail "missing inventory: $INVENTORY"
[[ -f "$PAYLOAD/LICENSES.sha256" ]] || fail 'payload has no LICENSES.sha256 manifest'

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

payload_manifest=$(value payload_manifest)
dependency_count=$(value dependency_count)
[[ "$dependency_count" =~ ^[1-9][0-9]*$ ]] || fail "invalid dependency count"

declare -A manifest_digest=()
while IFS= read -r line; do
    [[ "$line" =~ ^([0-9a-f]{64})[[:space:]][[:space:]]([^[:space:]]+)$ ]] ||
        fail 'invalid manifest line'
    manifest_digest[${BASH_REMATCH[2]}]=${BASH_REMATCH[1]}
done <"$PAYLOAD/$payload_manifest"

declare -A declared_notice=()

# Every declared dependency notice must be shipped and checksummed. The message
# names the dependency that is missing, so a failure says what to fix.
require_notice() {
    local dependency=$1
    local kind=$2
    local path=$3
    local digest

    [[ -n "$path" ]] || fail "dependency ${dependency} declares no ${kind}"
    [[ "$path" != /* && "$path" != *..* && "$path" != *\\* ]] ||
        fail "dependency ${dependency} has an unsafe ${kind}: $path"
    [[ -f "$PAYLOAD/$path" && ! -L "$PAYLOAD/$path" ]] ||
        fail "dependency ${dependency} ${kind} missing from the payload: $path"
    digest=${manifest_digest[$path]:-}
    [[ -n "$digest" ]] ||
        fail "dependency ${dependency} ${kind} is not covered by ${payload_manifest}: $path"
    [[ "$(sha256sum "$PAYLOAD/$path" | awk '{print $1}')" == "$digest" ]] ||
        fail "dependency ${dependency} ${kind} digest mismatch: $path"
    declared_notice[$path]=1
}

declare -A components=()
for ((index = 1; index <= dependency_count; ++index)); do
    prefix="dependency_${index}_"
    id=$(value "${prefix}id")
    notice=$(value "${prefix}notice")
    election=$(optional "${prefix}election")
    election_notice=$(optional "${prefix}election_notice")

    components[$id]=$(value "${prefix}component")
    require_notice "$id" 'notice' "$notice"
    if [[ -n "$election" ]]; then
        [[ -n "$election_notice" ]] ||
            fail "dependency ${id} records an election without an election notice"
        require_notice "$id" 'election notice' "$election_notice"
        # The shipped text must state the election the inventory records.
        grep -Fq -- "$election" "$PAYLOAD/$election_notice" ||
            fail "dependency ${id} election notice does not state ${election}: $election_notice"
        offered=$(optional "${prefix}license_offered")
        if [[ -n "$offered" ]]; then
            grep -Fq -- "$offered" "$PAYLOAD/$election_notice" ||
                fail "dependency ${id} election notice does not state the offered license: $election_notice"
        fi
        grep -Fq -- "${components[$id]}" "$PAYLOAD/$election_notice" ||
            fail "dependency ${id} election notice does not name ${components[$id]}: $election_notice"
    elif [[ -n "$election_notice" ]]; then
        fail "dependency ${id} ships an election notice without an election"
    fi
done

# The appliance's own licensing documents are part of the same contract.
for key in appliance_license_notice appliance_licensing_terms \
    appliance_commercial_terms; do
    path=$(value "$key")
    [[ -f "$PAYLOAD/$path" && ! -L "$PAYLOAD/$path" ]] ||
        fail "appliance ${key} missing from the payload: $path"
    [[ -n "${manifest_digest[$path]:-}" ]] ||
        fail "appliance ${key} is not covered by ${payload_manifest}: $path"
    declared_notice[$path]=1
done

# A notice that ships without being declared is a dependency nobody inventoried.
if [[ -d "$PAYLOAD/notices" ]]; then
    while IFS= read -r path; do
        [[ -n "${declared_notice[notices/$path]:-}" ]] ||
            fail "payload ships an undeclared notice: notices/$path"
    done < <(cd "$PAYLOAD/notices" && find . -type f -printf '%P\n' | sort)
fi

echo "License notices verification: OK (${dependency_count} declared dependencies)"
