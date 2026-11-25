#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
export LC_ALL=C
export LANG=C
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

bash scripts/verify-third-party-license-docs.sh

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
cp -a third_party "$TEST_DIR/third_party"

expect_reject() {
    if HFOC_THIRD_PARTY_ROOT="$1" \
        bash scripts/verify-third-party-license-docs.sh >/dev/null 2>&1; then
        echo "Third-party license fixture unexpectedly accepted: $1" >&2
        exit 1
    fi
}

printf '\nmutation\n' >>"$TEST_DIR/third_party/lwip/COPYING"
expect_reject "$TEST_DIR/third_party"

fresh_fixture() {
    local name=$1
    rm -rf "$TEST_DIR/$name"
    cp -a third_party "$TEST_DIR/$name"
    printf '%s' "$TEST_DIR/$name"
}

fixture=$(fresh_fixture missing-limine)
rm "$fixture/limine/COPYING"
expect_reject "$fixture"

fixture=$(fresh_fixture extra-limine)
printf 'extra\n' >"$fixture/limine/EXTRA"
expect_reject "$fixture"

fixture=$(fresh_fixture linked-limine)
rm "$fixture/limine/COPYING"
ln -s /etc/passwd "$fixture/limine/COPYING"
expect_reject "$fixture"

fixture=$(fresh_fixture duplicate-lock)
grep -E '^[0-9a-f]{64}  ' "$fixture/LICENSE-DOCUMENTS.sha256" | head -n 1 \
    >>"$fixture/LICENSE-DOCUMENTS.sha256"
expect_reject "$fixture"

fixture=$(fresh_fixture invalid-digest)
sed -i '0,/^[0-9a-f]\{64\}  /s//0000000000000000000000000000000000000000000000000000000000000000  /' \
    "$fixture/LICENSE-DOCUMENTS.sha256"
expect_reject "$fixture"

fixture=$(fresh_fixture false-scope)
sed -i 's/Third-party components are excluded/Third-party components are included/' \
    "$fixture/LICENSES.md"
expect_reject "$fixture"

echo 'Third-party license documents host test: OK'
