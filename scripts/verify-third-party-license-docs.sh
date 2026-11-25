#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C
REPO_ROOT=$(git rev-parse --show-toplevel)
THIRD_PARTY_ROOT=${HFOC_THIRD_PARTY_ROOT:-$REPO_ROOT/third_party}
LOCK=${HFOC_THIRD_PARTY_LOCK:-$THIRD_PARTY_ROOT/LICENSE-DOCUMENTS.sha256}
INVENTORY=$THIRD_PARTY_ROOT/LICENSES.md
EXPECTED_ARCHIVE=433ac5933ef7918b6a6751db0a81e47fa122581dfb32430bc257be686f3906e5
EXPECTED_SOURCE_COMMIT=8c8a688776735b2b2d12683a032e442583d361db
EXPECTED_SOURCE_TREE=220df4be15f25bf0e241a9c7a0a51843d62e5cec

fail() { echo "Third-party license documents: $*" >&2; exit 1; }
[[ -f "$LOCK" && ! -L "$LOCK" ]] || fail 'missing lock'
[[ -f "$INVENTORY" && ! -L "$INVENTORY" ]] || fail 'missing inventory'
[[ $(grep -c '^license_documents_version=1$' "$LOCK") -eq 1 ]] || fail 'invalid lock version'
[[ $(grep -c '^limine_version=12\.3\.2$' "$LOCK") -eq 1 ]] || fail 'invalid Limine version'
[[ $(grep -c "^limine_archive_sha256=${EXPECTED_ARCHIVE}$" "$LOCK") -eq 1 ]] || fail 'invalid Limine archive digest'
[[ $(grep -c "^limine_source_commit=${EXPECTED_SOURCE_COMMIT}$" "$LOCK") -eq 1 ]] || fail 'invalid Limine source commit'
[[ $(grep -c "^limine_source_tree=${EXPECTED_SOURCE_TREE}$" "$LOCK") -eq 1 ]] || fail 'invalid Limine source tree'
find "$THIRD_PARTY_ROOT/limine" -type l -print -quit | grep -q . && fail 'Limine notice symlink'
[[ -f "$THIRD_PARTY_ROOT/lwip/COPYING" && ! -L "$THIRD_PARTY_ROOT/lwip/COPYING" ]] || fail 'missing lwIP COPYING'
[[ -f "$THIRD_PARTY_ROOT/mbedtls/LICENSE" && ! -L "$THIRD_PARTY_ROOT/mbedtls/LICENSE" ]] || fail 'missing mbedTLS LICENSE'

declare -A seen=()
locked_paths=()
while IFS= read -r line; do
    [[ "$line" =~ ^([0-9a-f]{64})[[:space:]][[:space:]]((limine|lwip|mbedtls)/[^[:space:]]+)$ ]] || continue
    digest=${BASH_REMATCH[1]}
    path=${BASH_REMATCH[2]}
    [[ "$path" != /* && "$path" != *'..'* && "$path" != *\\* ]] || fail "unsafe path: $path"
    [[ -z ${seen[$path]:-} ]] || fail "duplicate path: $path"
    seen[$path]=1
    locked_paths+=("$path")
    [[ -f "$THIRD_PARTY_ROOT/$path" && ! -L "$THIRD_PARTY_ROOT/$path" ]] || fail "missing or linked: $path"
    actual=$(sha256sum "$THIRD_PARTY_ROOT/$path" | awk '{print $1}')
    [[ "$actual" == "$digest" ]] || fail "digest mismatch: $path"
done <"$LOCK"
(( ${#locked_paths[@]} >= 5 )) || fail 'notice lock is incomplete'

mapfile -t actual_paths < <(
    find "$THIRD_PARTY_ROOT/limine" -type f -printf 'limine/%P\n'
    printf 'lwip/COPYING\n'
    printf 'mbedtls/LICENSE\n'
)
locked_set=$(printf '%s\n' "${locked_paths[@]}" | sort)
actual_set=$(printf '%s\n' "${actual_paths[@]}" | sort)
[[ "$locked_set" == "$actual_set" ]] || fail 'locked notice path set differs from real path set'

for text in 'Limine 12.3.2' 'BSD-2-Clause' \
    'lwIP STABLE-2_2_1_RELEASE' 'BSD-3-Clause' \
    'mbedTLS mbedtls-3.6.7' 'Apache-2.0 OR GPL-2.0-or-later' \
    'Third-party components are excluded from the HFOC dual-license grant.'; do
    grep -Fq -- "$text" "$INVENTORY" || fail "inventory lacks: $text"
done

echo 'Third-party license document verification: OK'
