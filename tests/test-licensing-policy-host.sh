#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

required=(
    LICENSE
    LICENSES/MIT-legacy.txt
    LICENSING.md
    COMMERCIAL-LICENSING.md
    CONTRIBUTOR_LICENSE_AGREEMENT.md
    CONTRIBUTING.md
    docs/legal/HFOC-OWNERSHIP-AUDIT.md
    scripts/verify-licensing-policy.sh
)

for path in "${required[@]}"; do
    [[ -f "$path" ]] || {
        echo "Missing licensing policy file: $path" >&2
        exit 1
    }
done

bash scripts/verify-licensing-policy.sh

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

make_fixture() {
    local name=$1
    local dst="$TEST_DIR/$name"
    mkdir -p "$dst/LICENSES" "$dst/docs/legal"
    cp LICENSE "$dst/LICENSE"
    cp LICENSES/MIT-legacy.txt "$dst/LICENSES/MIT-legacy.txt"
    cp LICENSING.md COMMERCIAL-LICENSING.md \
       CONTRIBUTOR_LICENSE_AGREEMENT.md CONTRIBUTING.md "$dst/"
    cp docs/legal/HFOC-OWNERSHIP-AUDIT.md "$dst/docs/legal/"
    printf '%s' "$dst"
}

expect_reject() {
    local root=$1
    if HFOC_LICENSE_ROOT="$root" bash scripts/verify-licensing-policy.sh \
        >/dev/null 2>&1; then
        echo "Licensing fixture unexpectedly accepted: $root" >&2
        exit 1
    fi
}

fixture=$(make_fixture missing-commercial)
rm "$fixture/COMMERCIAL-LICENSING.md"
expect_reject "$fixture"

fixture=$(make_fixture mutated-agpl)
printf '\nmutation\n' >>"$fixture/LICENSE"
expect_reject "$fixture"

fixture=$(make_fixture mutated-mit)
printf '\nmutation\n' >>"$fixture/LICENSES/MIT-legacy.txt"
expect_reject "$fixture"

fixture=$(make_fixture missing-boundary)
sed -i '/^Legacy MIT boundary:/d' "$fixture/LICENSING.md"
expect_reject "$fixture"

fixture=$(make_fixture commercial-false-grant)
sed -i 's/grants no commercial-license rights/grants commercial-license rights/' \
    "$fixture/COMMERCIAL-LICENSING.md"
expect_reject "$fixture"

echo 'Licensing policy host test: OK'
