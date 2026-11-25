#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
THIRD_PARTY_DIR=${1:-$ROOT/third_party}
OUTPUT=${2:-$THIRD_PARTY_DIR/LOCKS.sha256}
LWIP_DIR="$THIRD_PARTY_DIR/lwip"
LOCK_SOURCE="$THIRD_PARTY_DIR/LOCKS.sha256"

[[ -d "$LWIP_DIR" ]] || {
    echo "Missing vendored lwIP tree: $LWIP_DIR" >&2
    exit 1
}
[[ -f "$LWIP_DIR/COPYING" ]] || {
    echo "Missing lwIP COPYING: $LWIP_DIR/COPYING" >&2
    exit 1
}
if find "$LWIP_DIR" -type l -print -quit | grep -q .; then
    echo "lwIP tree contains a symlink" >&2
    exit 1
fi

file_count=$(find "$LWIP_DIR" -type f -print | wc -l)
if [[ ! "$file_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid lwIP file count: $file_count" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
tmp="${OUTPUT}.tmp.$$"
trap 'rm -f "$tmp"' EXIT
{
    if [[ -f "$LOCK_SOURCE" ]]; then
        mbedtls_section=$(awk \
            '/^mbedtls_lock_version=1$/{p=1} /^lwip_lock_version=1$/{p=0} p{print}' \
            "$LOCK_SOURCE")
        if [[ -n "$mbedtls_section" ]]; then
            printf '%s\n' "$mbedtls_section"
        fi
    fi
    printf 'lwip_lock_version=1\n'
    printf 'upstream_name=lwIP\n'
    printf 'upstream_version=STABLE-2_2_1_RELEASE\n'
    printf 'upstream_commit=77dcd25a72509eb83f72b033d219b1d40cd8eb95\n'
    printf 'upstream_url=https://github.com/lwip-tcpip/lwip/archive/refs/tags/STABLE-2_2_1_RELEASE.tar.gz\n'
    printf 'upstream_sha256=ce0b7461c0ad9602c376f0bf07c5eb7253b48c7bf66f011c6bf3e2a96731c539\n'
    printf 'upstream_license=BSD-3-Clause\n'
    printf 'file_count=%s\n' "$file_count"
    printf 'files_begin\n'
    while IFS= read -r -d '' file; do
        relative=${file#"$THIRD_PARTY_DIR/"}
        digest=$(sha256sum "$file" | awk '{print $1}')
        printf '%s  %s  BSD-3-Clause\n' "$digest" "$relative"
    done < <(find "$LWIP_DIR" -type f -print0 | sort -z)
} >"$tmp"
mv "$tmp" "$OUTPUT"
trap - EXIT

echo "lwIP source lock: $OUTPUT"
