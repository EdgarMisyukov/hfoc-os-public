#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
SOURCE=${HFOC_LIMINE_LICENSE_SOURCE:-$ROOT/deps/limine-source}
DEST=$ROOT/third_party/limine
LOCK=$ROOT/third_party/LICENSE-DOCUMENTS.sha256
ARCHIVE_SHA=433ac5933ef7918b6a6751db0a81e47fa122581dfb32430bc257be686f3906e5
SOURCE_COMMIT=8c8a688776735b2b2d12683a032e442583d361db
SOURCE_TREE=220df4be15f25bf0e241a9c7a0a51843d62e5cec

declare -A expected_blobs=(
    [COPYING]=a59751f998ce46bf9815cb9fffc3fe06b47d85d6
    [3RDPARTY.md]=974b997573c9e7afcbc639bf3a64a90e23e6076b
    [LICENSES/LicenseRef-scancode-bsd-no-disclaimer-unmodified.txt]=ff0021a06d9fe77b68b13e5ede6c4f7135b9fa8d
)

for path in "${!expected_blobs[@]}"; do
    [[ -f "$SOURCE/$path" && ! -L "$SOURCE/$path" ]] || {
        echo "Missing or linked Limine notice: $path" >&2
        exit 1
    }
    actual_blob=$(git hash-object "$SOURCE/$path")
    [[ "$actual_blob" == "${expected_blobs[$path]}" ]] || {
        echo "Unexpected Limine notice blob: $path" >&2
        exit 1
    }
done

[[ -f "$ROOT/third_party/mbedtls/LICENSE" && \
    ! -L "$ROOT/third_party/mbedtls/LICENSE" ]] || {
    echo "Missing or linked mbedTLS LICENSE" >&2
    exit 1
}

tmp=$(mktemp -d "$ROOT/third_party/.limine-license.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
cp "$SOURCE/COPYING" "$tmp/COPYING"
cp "$SOURCE/3RDPARTY.md" "$tmp/3RDPARTY.md"
mkdir "$tmp/LICENSES"
cp "$SOURCE/LICENSES/LicenseRef-scancode-bsd-no-disclaimer-unmodified.txt" \
    "$tmp/LICENSES/LicenseRef-scancode-bsd-no-disclaimer-unmodified.txt"
find "$tmp" -type l -print -quit | grep -q . && {
    echo 'Limine notice tree contains a symlink' >&2
    exit 1
}
rm -rf "$DEST"
mv "$tmp" "$DEST"
trap - EXIT

lock_tmp="${LOCK}.tmp.$$"
{
    printf 'license_documents_version=1\n'
    printf 'limine_version=12.3.2\n'
    printf 'limine_archive_sha256=%s\n' "$ARCHIVE_SHA"
    printf 'limine_source_commit=%s\n' "$SOURCE_COMMIT"
    printf 'limine_source_tree=%s\n' "$SOURCE_TREE"
    find "$DEST" -type f -printf '%P\n' | LC_ALL=C sort | while read -r path; do
        digest=$(sha256sum "$DEST/$path" | awk '{print $1}')
        printf '%s  limine/%s\n' "$digest" "$path"
    done
    digest=$(sha256sum "$ROOT/third_party/lwip/COPYING" | awk '{print $1}')
    printf '%s  lwip/COPYING\n' "$digest"
    digest=$(sha256sum "$ROOT/third_party/mbedtls/LICENSE" | awk '{print $1}')
    printf '%s  mbedtls/LICENSE\n' "$digest"
} >"$lock_tmp"
mv "$lock_tmp" "$LOCK"
