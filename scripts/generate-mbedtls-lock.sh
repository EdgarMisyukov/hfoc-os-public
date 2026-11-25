#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
THIRD_PARTY_DIR=${1:-$ROOT/third_party}
OUTPUT=${2:-$THIRD_PARTY_DIR/LOCKS.sha256}
MBEDTLS_DIR="$THIRD_PARTY_DIR/mbedtls"
LOCK_SOURCE="$THIRD_PARTY_DIR/LOCKS.sha256"

[[ -d "$MBEDTLS_DIR" ]] || {
    echo "Missing vendored mbedTLS tree: $MBEDTLS_DIR" >&2
    exit 1
}
[[ -f "$MBEDTLS_DIR/LICENSE" ]] || {
    echo "Missing mbedTLS LICENSE: $MBEDTLS_DIR/LICENSE" >&2
    exit 1
}
if find "$MBEDTLS_DIR" -type l -print -quit | grep -q .; then
    echo "mbedTLS tree contains a symlink" >&2
    exit 1
fi

file_count=$(find "$MBEDTLS_DIR" -type f -print | wc -l)
if [[ ! "$file_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Invalid mbedTLS file count: $file_count" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUTPUT")"
tmp="${OUTPUT}.tmp.$$"
trap 'rm -f "$tmp"' EXIT
{
    printf 'mbedtls_lock_version=1\n'
    printf 'upstream_name=mbedTLS\n'
    printf 'upstream_version=mbedtls-3.6.7\n'
    printf 'upstream_commit=068ff080b369adfac81509f9b57b2afabaf82dc5\n'
    printf 'upstream_url=https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.7/mbedtls-3.6.7.tar.bz2\n'
    printf 'upstream_sha256=a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6\n'
    printf 'upstream_license=Apache-2.0 OR GPL-2.0-or-later\n'
    printf 'file_count=%s\n' "$file_count"
    printf 'mbedtls_files_begin\n'
    while IFS= read -r -d '' file; do
        relative=${file#"$THIRD_PARTY_DIR/"}
        digest=$(sha256sum "$file" | awk '{print $1}')
        printf '%s  %s  Apache-2.0 OR GPL-2.0-or-later\n' \
            "$digest" "$relative"
    done < <(find "$MBEDTLS_DIR" -type f -print0 | sort -z)
    if [[ -f "$LOCK_SOURCE" ]]; then
        awk '/^lwip_lock_version=1$/{p=1} p{print}' "$LOCK_SOURCE"
    fi
} >"$tmp"
mv "$tmp" "$OUTPUT"
trap - EXIT

echo "mbedTLS source lock: $OUTPUT"
