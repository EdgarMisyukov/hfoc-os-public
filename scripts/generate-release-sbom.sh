#!/usr/bin/env bash
# ITEM-380 release SBOM generator (SPDX 2.3 tag-value).
#
# The SBOM is generated offline from the artifacts the repository already
# pins: third_party/LICENSES.md (dependency identity, archive digest, license
# family and text), third_party/LOCKS.sha256 (the exact vendored file list and
# per-file SHA-256) and the release artifact manifest (source revision).  No
# network access and no package manager are involved, and the document is
# deterministic: its Created timestamp comes from SOURCE_DATE_EPOCH or from the
# source commit time, never from the wall clock.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}
OUTPUT=${1:-build/release/sbom/$RELEASE_ID.spdx}
INVENTORY=third_party/LICENSES.md
LOCKS=third_party/LOCKS.sha256

[[ -f "$INVENTORY" && -f "$LOCKS" ]] || {
    echo 'Release SBOM: the license inventory or the lock file is missing' >&2
    exit 1
}

revision=$(git rev-parse HEAD 2>/dev/null || echo unknown)
version=$(cat VERSION)
if [[ -n "${SOURCE_DATE_EPOCH:-}" ]]; then
    created_epoch=$SOURCE_DATE_EPOCH
else
    created_epoch=$(git log -1 --format=%ct 2>/dev/null || echo 0)
fi
[[ "$created_epoch" =~ ^[0-9]+$ ]] || {
    echo 'Release SBOM: invalid SOURCE_DATE_EPOCH' >&2
    exit 1
}
created=$(date -u -d "@$created_epoch" +%Y-%m-%dT%H:%M:%SZ)

spdx_slug() {
    printf 'SPDXRef-Package-%s' "$(printf '%s' "$1" |
        tr '[:upper:]' '[:lower:]' | tr -c 'a-z0-9.-' '-')"
}

emit_package() {
    local name=$1
    local release=$2
    local commit=$3
    local url=$4
    local archive_sha=$5
    local family=$6
    local license_text=$7
    local file_count=$8
    local slug
    local concluded=$family

    slug=$(spdx_slug "$name")
    if [[ "$name" == "mbedTLS" ]]; then
        # ITEM-382 requires the election to be recorded in the inventory and in
        # the SBOM: the vendored mbedTLS is distributed under the Apache-2.0
        # arm of its dual license.
        concluded=Apache-2.0
    fi
    {
        printf 'PackageName: %s\n' "$name"
        printf 'SPDXID: %s\n' "$slug"
        printf 'PackageVersion: %s\n' "$release"
        printf 'PackageDownloadLocation: %s\n' "$url"
        printf 'FilesAnalyzed: true\n'
        printf 'PackageChecksum: SHA256: %s\n' "$archive_sha"
        printf 'PackageLicenseConcluded: %s\n' "$concluded"
        printf 'PackageLicenseDeclared: %s\n' "$family"
        printf 'PackageCopyrightText: NOASSERTION\n'
        printf 'PackageComment: commit=%s license_text=%s vendored_files=%s election=%s\n' \
            "$commit" "$license_text" "$file_count" "$concluded"
        printf '\n'
    }
}

emit_files() {
    local prefix=$1
    local package_slug=$2
    local digest
    local path
    local spdx_id

    while IFS= read -r line; do
        [[ "$line" =~ ^([0-9a-f]{64})[[:space:]][[:space:]]([^[:space:]]+) ]] || continue
        digest=${BASH_REMATCH[1]}
        path=${BASH_REMATCH[2]}
        [[ "$path" == "$prefix"/* ]] || continue
        spdx_id="SPDXRef-File-$(printf '%s' "$path" | tr -c 'a-zA-Z0-9.-' '-')"
        printf 'FileName: ./third_party/%s\n' "$path"
        printf 'SPDXID: %s\n' "$spdx_id"
        printf 'FileChecksum: SHA256: %s\n' "$digest"
        printf 'LicenseConcluded: NOASSERTION\n'
        printf 'LicenseInfoInFile: NOASSERTION\n'
        printf 'FileCopyrightText: NOASSERTION\n'
        printf 'Relationship: %s CONTAINS %s\n' "$package_slug" "$spdx_id"
        printf '\n'
    done <"$LOCKS"
}

mkdir -p "$(dirname "$OUTPUT")"
{
    printf 'SPDXVersion: SPDX-2.3\n'
    printf 'DataLicense: CC0-1.0\n'
    printf 'SPDXID: SPDXRef-DOCUMENT\n'
    printf 'DocumentName: %s\n' "$RELEASE_ID"
    printf 'DocumentNamespace: https://hfoc.invalid/spdx/%s-%s\n' \
        "$RELEASE_ID" "$revision"
    printf 'Creator: Tool: hfoc-generate-release-sbom-1\n'
    printf 'Created: %s\n' "$created"
    printf 'DocumentComment: offline SBOM generated from %s and %s; product_version=%s\n' \
        "$INVENTORY" "$LOCKS" "$version"
    printf '\n'

    emit_package 'HFOC OS' "$version" "$revision" \
        'NOASSERTION' \
        "$(sha256sum "$INVENTORY" | awk '{print $1}')" \
        'AGPL-3.0-only OR LicenseRef-HFOC-Commercial' \
        'LICENSING.md' '0'
    printf 'Relationship: SPDXRef-DOCUMENT DESCRIBES SPDXRef-Package-hfoc-os\n\n'

    # Parse the inventory into one dependency block per section.
    awk '
        /^## / {
            name = $2
            release = $3
            print "DEPENDENCY\t" name "\t" release
            next
        }
        /^- Commit: / { gsub(/`/, "", $3); print "FIELD\tcommit\t" $3 }
        /^- URL: / { gsub(/`/, "", $3); print "FIELD\turl\t" $3 }
        /^- Archive SHA-256: / { gsub(/`/, "", $4); print "FIELD\tsha\t" $4 }
        /^- License family: / {
            line = $0
            sub(/^- License family: `/, "", line)
            sub(/`$/, "", line)
            print "FIELD\tfamily\t" line
        }
        /^- License text: / { gsub(/`/, "", $4); print "FIELD\ttext\t" $4 }
        /^- Covered files: / {
            line = $0
            sub(/.*file_count=/, "", line)
            sub(/`.*/, "", line)
            print "FIELD\tcount\t" line
        }
    ' "$INVENTORY" >"$OUTPUT.deps"

    name=
    release=
    commit=
    url=
    archive_sha=
    family=
    license_text=
    file_count=
    flush_dependency() {
        [[ -n "$name" ]] || return 0
        emit_package "$name" "$release" "$commit" "$url" "$archive_sha" \
            "$family" "$license_text" "$file_count"
        local slug
        slug=$(spdx_slug "$name")
        printf 'Relationship: SPDXRef-DOCUMENT DESCRIBES %s\n' "$slug"
        printf 'Relationship: SPDXRef-Package-hfoc-os DEPENDS_ON %s\n\n' "$slug"
        case "$name" in
            mbedTLS) emit_files 'mbedtls' "$slug" ;;
            lwIP) emit_files 'lwip' "$slug" ;;
        esac
    }
    while IFS=$'\t' read -r kind key value; do
        case "$kind" in
            DEPENDENCY)
                flush_dependency
                name=$key
                release=$value
                commit=
                url=
                archive_sha=
                family=
                license_text=
                file_count=0
                ;;
            FIELD)
                case "$key" in
                    commit) commit=$value ;;
                    url) url=$value ;;
                    sha) archive_sha=$value ;;
                    family) family=$value ;;
                    text) license_text=$value ;;
                    count) file_count=$value ;;
                esac
                ;;
        esac
    done <"$OUTPUT.deps"
    flush_dependency
    rm -f "$OUTPUT.deps"
} >"$OUTPUT"

echo "Release SBOM: $OUTPUT"
