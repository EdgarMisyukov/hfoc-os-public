#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

bash scripts/install-license-payload.sh "$TEST_DIR/release"
bash scripts/verify-license-payload.sh "$TEST_DIR/release/licenses"
bash scripts/verify-license-notices.sh "$TEST_DIR/release/licenses"
test -f "$TEST_DIR/release/licenses/AGPL-3.0-only.txt"
test -f "$TEST_DIR/release/licenses/LICENSES.sha256"
test -f "$TEST_DIR/release/licenses/third_party/limine/COPYING"
test -f "$TEST_DIR/release/licenses/third_party/lwip/COPYING"
test -f "$TEST_DIR/release/licenses/third_party/mbedtls/LICENSE"
test -f "$TEST_DIR/release/licenses/notices/mbedtls-Apache-2.0-election.txt"

fresh_payload() {
    local name=$1
    bash scripts/install-license-payload.sh "$TEST_DIR/$name" >/dev/null
    printf '%s' "$TEST_DIR/$name/licenses"
}
expect_payload_reject() {
    if bash scripts/verify-license-payload.sh "$1" >/dev/null 2>&1; then
        echo "License payload fixture unexpectedly accepted: $1" >&2
        exit 1
    fi
}
# ITEM-382: a broken notices contract must fail and name the dependency, so the
# failure tells a release engineer what to ship rather than only that the
# payload is wrong.
expect_notices_reject() {
    local label=$1
    local payload=$2
    local dependency=$3
    local log="$TEST_DIR/notices-$label.log"
    local status=0

    set +e
    HFOC_LICENSE_INVENTORY=${HFOC_LICENSE_INVENTORY:-$ROOT/release/hfoc-license-inventory.txt} \
        bash scripts/verify-license-notices.sh "$payload" \
        >"$log" 2>&1
    status=$?
    set -e
    if (( status == 0 )); then
        echo "License notices fixture unexpectedly accepted: $label" >&2
        exit 1
    fi
    if ! grep -Fq -- "$dependency" "$log"; then
        echo "License notices fixture did not name $dependency: $label" >&2
        cat "$log" >&2
        exit 1
    fi
}

payload=$(fresh_payload missing-policy); rm "$payload/LICENSING.md"; expect_payload_reject "$payload"
payload=$(fresh_payload mutated-agpl); printf '\nmutation\n' >>"$payload/AGPL-3.0-only.txt"; expect_payload_reject "$payload"
payload=$(fresh_payload extra-file); printf 'extra\n' >"$payload/extra.txt"; expect_payload_reject "$payload"
payload=$(fresh_payload missing-limine); rm "$payload/third_party/limine/COPYING"; expect_payload_reject "$payload"
payload=$(fresh_payload missing-mbedtls); rm "$payload/third_party/mbedtls/LICENSE"; expect_payload_reject "$payload"
payload=$(fresh_payload symlink); ln -s /etc/passwd "$payload/linked-notice"; expect_payload_reject "$payload"
payload=$(fresh_payload bad-manifest); sed -i '1s/^[0-9a-f]/0/' "$payload/LICENSES.sha256"; expect_payload_reject "$payload"

# ITEM-382 notices contract: every declared dependency must ship its notice and
# the payload checksum manifest must cover it.
payload=$(fresh_payload notices-missing-lwip); rm "$payload/third_party/lwip/COPYING"
expect_notices_reject notices-missing-lwip "$payload" lwip
payload=$(fresh_payload notices-renamed-election)
mv "$payload/notices/mbedtls-Apache-2.0-election.txt" \
    "$payload/notices/mbedtls-election.txt"
expect_notices_reject notices-renamed-election "$payload" mbedtls
payload=$(fresh_payload notices-uncovered)
sed -i '\|  third_party/mbedtls/LICENSE$|d' "$payload/LICENSES.sha256"
expect_notices_reject notices-uncovered "$payload" mbedtls
payload=$(fresh_payload notices-undeclared)
printf 'undeclared\n' >"$payload/notices/undeclared-notice.txt"
expect_notices_reject notices-undeclared "$payload" undeclared

# A dependency added to the inventory without shipping its notice must fail the
# contract and name the dependency that has no notice.
payload=$(fresh_payload notices-new-dependency)
sed 's/^dependency_count=4$/dependency_count=5/' \
    release/hfoc-license-inventory.txt >"$TEST_DIR/inventory-new-dependency.txt"
cat >>"$TEST_DIR/inventory-new-dependency.txt" <<'EOF'
dependency_5_id=phantom
dependency_5_component=Phantom
dependency_5_version=1.0.0
dependency_5_license=MIT
dependency_5_notice=notices/phantom.txt
EOF
if HFOC_LICENSE_INVENTORY="$TEST_DIR/inventory-new-dependency.txt" \
        bash scripts/verify-license-notices.sh "$payload" \
        >"$TEST_DIR/notices-new-dependency.log" 2>&1; then
    echo 'a declared dependency without a shipped notice was accepted' >&2
    exit 1
fi
grep -Fq -- 'phantom' "$TEST_DIR/notices-new-dependency.log"

# The declared inventory itself must not accept that dependency either: it
# declares a notice the repository does not carry.
if bash scripts/verify-license-inventory.sh \
        "$TEST_DIR/inventory-new-dependency.txt" >/dev/null 2>&1; then
    echo 'the inventory verifier accepted an unshippable dependency' >&2
    exit 1
fi

echo 'License payload host test: OK'
