#!/usr/bin/env bash
# ITEM-380 strict verifier for the release SBOM (SPDX 2.3 tag-value).
#
# The SBOM must describe the product and every vendored dependency with the
# identity, archive digest and license family the repository pins, must record
# the mbedTLS Apache-2.0 election, and must list every locked vendored file
# with exactly the SHA-256 in third_party/LOCKS.sha256. A missing package, a
# missing election, a foreign source revision and a tampered file digest are
# refused.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

if (( $# < 1 || $# > 2 )); then
    echo "usage: $0 <sbom.spdx> [source-revision]" >&2
    exit 2
fi
SBOM=$1
REVISION=${2:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}
INVENTORY=third_party/LICENSES.md
LOCKS=third_party/LOCKS.sha256
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}

[[ -f "$SBOM" ]] || {
    echo "Missing release SBOM: $SBOM" >&2
    exit 1
}
[[ -f "$INVENTORY" && -f "$LOCKS" ]] || {
    echo "Release SBOM verification: the license inventory or the lock file is missing" >&2
    exit 1
}

required_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}:" "$SBOM") -ne 1 ]]; then
        echo "Release SBOM is missing ${key}" >&2
        exit 1
    fi
    value=$(sed -n "s/^${key}: //p" "$SBOM")
    if [[ -z "$value" ]]; then
        echo "Release SBOM has empty ${key}" >&2
        exit 1
    fi
    printf '%s' "$value"
}

[[ "$(required_value SPDXVersion)" == "SPDX-2.3" ]] || {
    echo "Release SBOM has an unsupported SPDX version" >&2
    exit 1
}
[[ "$(required_value DataLicense)" == "CC0-1.0" ]] || {
    echo "Release SBOM has an unsupported data license" >&2
    exit 1
}
[[ "$(required_value DocumentName)" == "$RELEASE_ID" ]] || {
    echo "Release SBOM names another document" >&2
    exit 1
}
namespace=$(required_value DocumentNamespace)
[[ "$namespace" == *"-${REVISION}" ]] || {
    echo "Release SBOM does not describe the current source revision" >&2
    exit 1
}
creator=$(required_value Creator)
[[ "$creator" == Tool:\ hfoc-generate-release-sbom-* ]] || {
    echo "Release SBOM was not produced by the declared generator" >&2
    exit 1
}
created=$(required_value Created)
[[ "$created" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$ ]] || {
    echo "Release SBOM has an invalid creation timestamp" >&2
    exit 1
}

# One package block per inventory dependency, with the pinned identity.
while IFS=$'\t' read -r name release archive_sha family file_count; do
    [[ -n "$name" ]] || continue
    block=$(awk -v name="$name" '
        /^PackageName: / { inside = ($0 == "PackageName: " name) }
        inside { print }
    ' "$SBOM")
    if [[ -z "$block" ]]; then
        echo "Release SBOM is missing the ${name} package" >&2
        exit 1
    fi
    for expected in "PackageVersion: ${release}" \
                    "PackageChecksum: SHA256: ${archive_sha}" \
                    "PackageLicenseDeclared: ${family}"; do
        if ! grep -Fqx -- "$expected" <<<"$block"; then
            echo "Release SBOM ${name} package is missing: ${expected}" >&2
            exit 1
        fi
    done
    if [[ "$name" == "mbedTLS" ]]; then
        grep -Fqx 'PackageLicenseConcluded: Apache-2.0' <<<"$block" || {
            echo "Release SBOM does not record the mbedTLS Apache-2.0 election" >&2
            exit 1
        }
        grep -Fq 'election=Apache-2.0' <<<"$block" || {
            echo "Release SBOM does not record the mbedTLS election in the package comment" >&2
            exit 1
        }
    elif ! grep -Fqx "PackageLicenseConcluded: ${family}" <<<"$block"; then
        echo "Release SBOM ${name} package does not conclude its declared license" >&2
        exit 1
    fi
    if ! grep -Fq "Relationship: SPDXRef-DOCUMENT DESCRIBES" "$SBOM"; then
        echo "Release SBOM describes no package" >&2
        exit 1
    fi
    if ! grep -Fq "DEPENDS_ON SPDXRef-Package-$(printf '%s' "$name" |
            tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9.-' '-')" "$SBOM"; then
        echo "Release SBOM does not relate the product to ${name}" >&2
        exit 1
    fi
    if [[ "$name" == "mbedTLS" || "$name" == "lwIP" ]]; then
        prefix=$(printf '%s' "$name" | tr '[:upper:]' '[:lower:]')
        locked=$(grep -c "^[0-9a-f]\{64\}  ${prefix}/" "$LOCKS" || true)
        if [[ "$locked" != "$file_count" ]]; then
            echo "Release SBOM file count for ${name} does not match the lock file" >&2
            exit 1
        fi
    fi
done < <(awk '
    /^## / {
        if (name != "") print name "\t" release "\t" sha "\t" family "\t" count
        name = $2; release = $3; sha = ""; family = ""; count = ""
        next
    }
    /^- Archive SHA-256: / { gsub(/`/, "", $4); sha = $4 }
    /^- License family: / {
        line = $0; sub(/^- License family: `/, "", line); sub(/`$/, "", line)
        family = line
    }
    /^- Covered files: / {
        line = $0; sub(/.*file_count=/, "", line); sub(/`.*/, "", line)
        count = line
    }
    END { if (name != "") print name "\t" release "\t" sha "\t" family "\t" count }
' "$INVENTORY")

# Every locked vendored file must appear with exactly its locked digest.
awk '
    /^FileName: / { file = $2 }
    /^FileChecksum: SHA256: / { digest = $3; print file "\t" digest }
' "$SBOM" >"$SBOM.files"
while IFS= read -r line; do
    [[ "$line" =~ ^([0-9a-f]{64})[[:space:]][[:space:]]([^[:space:]]+) ]] || continue
    digest=${BASH_REMATCH[1]}
    path=${BASH_REMATCH[2]}
    if ! grep -Fqx -- "./third_party/${path}"$'\t'"$digest" "$SBOM.files"; then
        echo "Release SBOM does not list ${path} with its locked digest" >&2
        rm -f "$SBOM.files"
        exit 1
    fi
done <"$LOCKS"
file_entries=$(grep -c '^FileName: ' "$SBOM" || true)
locked_entries=$(grep -c '^[0-9a-f]\{64\}  ' "$LOCKS" || true)
rm -f "$SBOM.files"
if [[ "$file_entries" != "$locked_entries" ]]; then
    echo "Release SBOM lists ${file_entries} files, expected ${locked_entries}" >&2
    exit 1
fi

echo "Release SBOM verification: OK"
