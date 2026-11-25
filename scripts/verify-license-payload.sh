#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C
ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
LICENSES_DIR=${1:-}
AGPL_SHA=d8a6cc31abc16b6748c7a21f21611f5a1ec33f67d22ca23d7da1c19b95496bee
[[ -n "$LICENSES_DIR" && -d "$LICENSES_DIR" && ! -L "$LICENSES_DIR" ]] || {
    echo 'Usage: verify-license-payload.sh LICENSES_DIR' >&2
    exit 2
}
fail() { echo "License payload: $*" >&2; exit 1; }

for path in AGPL-3.0-only.txt LICENSING.md MIT-legacy.txt \
    COMMERCIAL-LICENSING.md third_party/LICENSES.md \
    third_party/LICENSE-DOCUMENTS.sha256 third_party/lwip/COPYING \
    third_party/mbedtls/LICENSE \
    third_party/limine/COPYING third_party/limine/3RDPARTY.md \
    notices/mbedtls-Apache-2.0-election.txt \
    LICENSES.sha256; do
    [[ -f "$LICENSES_DIR/$path" && ! -L "$LICENSES_DIR/$path" ]] || fail "missing or linked: $path"
done
find "$LICENSES_DIR" -type l -print -quit | grep -q . && fail 'symlink found'

declare -A seen=()
locked_paths=()
while IFS= read -r line; do
    [[ "$line" =~ ^([0-9a-f]{64})[[:space:]][[:space:]]([^[:space:]]+)$ ]] || fail 'invalid manifest line'
    digest=${BASH_REMATCH[1]}
    path=${BASH_REMATCH[2]}
    [[ "$path" != /* && "$path" != *'..'* && "$path" != *\\* ]] || fail "unsafe path: $path"
    [[ -z ${seen[$path]:-} ]] || fail "duplicate path: $path"
    seen[$path]=1
    locked_paths+=("$path")
    [[ -f "$LICENSES_DIR/$path" && ! -L "$LICENSES_DIR/$path" ]] || fail "missing: $path"
    actual=$(sha256sum "$LICENSES_DIR/$path" | awk '{print $1}')
    [[ "$actual" == "$digest" ]] || fail "digest mismatch: $path"
done <"$LICENSES_DIR/LICENSES.sha256"

mapfile -t actual_paths < <(
    cd "$LICENSES_DIR"
    find . -type f ! -name LICENSES.sha256 -printf '%P\n' | sort
)
locked_set=$(printf '%s\n' "${locked_paths[@]}" | sort)
actual_set=$(printf '%s\n' "${actual_paths[@]}" | sort)
[[ "$locked_set" == "$actual_set" ]] || fail 'manifest path set differs from payload'
actual_agpl=$(sha256sum "$LICENSES_DIR/AGPL-3.0-only.txt" | awk '{print $1}')
[[ "$actual_agpl" == "$AGPL_SHA" ]] || fail 'canonical AGPL digest mismatch'

HFOC_THIRD_PARTY_ROOT="$LICENSES_DIR/third_party" \
HFOC_THIRD_PARTY_LOCK="$LICENSES_DIR/third_party/LICENSE-DOCUMENTS.sha256" \
    bash "$ROOT/scripts/verify-third-party-license-docs.sh" >/dev/null
# ITEM-382: the payload is forced to carry every declared dependency notice,
# and the payload manifest is forced to cover it.
bash "$ROOT/scripts/verify-license-notices.sh" "$LICENSES_DIR" >/dev/null
grep -Fq 'grants no commercial-license rights' \
    "$LICENSES_DIR/COMMERCIAL-LICENSING.md" || fail 'commercial no-grant notice missing'
echo 'License payload verification: OK'
