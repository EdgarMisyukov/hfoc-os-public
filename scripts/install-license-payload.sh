#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
RELEASE_ROOT=${1:-}
[[ -n "$RELEASE_ROOT" ]] || { echo 'Usage: install-license-payload.sh RELEASE_ROOT' >&2; exit 2; }
mkdir -p "$RELEASE_ROOT"
RELEASE_ROOT=$(realpath -e "$RELEASE_ROOT")
[[ "$RELEASE_ROOT" != / ]] || { echo 'Refusing root release directory' >&2; exit 2; }
DEST=$RELEASE_ROOT/licenses
payload_tmp=$(mktemp -d "$RELEASE_ROOT/.licenses.XXXXXX")
trap 'rm -rf "$payload_tmp"' EXIT

install -D -m 0644 "$ROOT/LICENSE" "$payload_tmp/AGPL-3.0-only.txt"
install -D -m 0644 "$ROOT/LICENSING.md" "$payload_tmp/LICENSING.md"
install -D -m 0644 "$ROOT/LICENSES/MIT-legacy.txt" "$payload_tmp/MIT-legacy.txt"
install -D -m 0644 "$ROOT/COMMERCIAL-LICENSING.md" "$payload_tmp/COMMERCIAL-LICENSING.md"
install -D -m 0644 "$ROOT/third_party/LICENSES.md" "$payload_tmp/third_party/LICENSES.md"
install -D -m 0644 "$ROOT/third_party/LICENSE-DOCUMENTS.sha256" \
    "$payload_tmp/third_party/LICENSE-DOCUMENTS.sha256"
install -D -m 0644 "$ROOT/third_party/lwip/COPYING" \
    "$payload_tmp/third_party/lwip/COPYING"
install -D -m 0644 "$ROOT/third_party/mbedtls/LICENSE" \
    "$payload_tmp/third_party/mbedtls/LICENSE"
while IFS= read -r path; do
    install -D -m 0644 "$ROOT/third_party/limine/$path" \
        "$payload_tmp/third_party/limine/$path"
done < <(cd "$ROOT/third_party/limine" && find . -type f -printf '%P\n' | sort)
# ITEM-382 election notices. Every file under notices/ is shipped, and
# scripts/verify-license-notices.sh refuses a notice that the declared
# dependency inventory does not cover.
while IFS= read -r path; do
    install -D -m 0644 "$ROOT/notices/$path" \
        "$payload_tmp/notices/$path"
done < <(cd "$ROOT/notices" && find . -type f -printf '%P\n' | sort)

find "$payload_tmp" -type l -print -quit | grep -q . && {
    echo 'License payload contains a symlink' >&2
    exit 1
}
(
    cd "$payload_tmp"
    find . -type f ! -name LICENSES.sha256 -printf '%P\n' | sort |
        while read -r path; do
            digest=$(sha256sum "$path" | awk '{print $1}')
            printf '%s  %s\n' "$digest" "$path"
        done
) >"$payload_tmp/LICENSES.sha256"

rm -rf "$DEST"
mv "$payload_tmp" "$DEST"
trap - EXIT
echo "License payload installed: $DEST"
