#!/usr/bin/env bash
# Host regression harness for the integrated installer/dual-boot/management
# acceptance gate.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
MATRIX=$TEST_DIR/matrix.txt

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"

run_verifier() {
    bash scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh "$MATRIX"
}

run_runner() {
    HFOC_INTEGRATED_ACCEPTANCE_MATRIX="$MATRIX" \
        bash scripts/test-hfoc-integrated-installer-dual-boot-management-acceptance.sh
}

expect_failure() {
    local label=$1
    local status=0
    shift

    set +e
    "$@" >/dev/null 2>&1
    status=$?
    set -e
    if (( status == 0 )); then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

if ! verifier_output=$(run_verifier); then
    echo 'canonical integrated acceptance verification failed' >&2
    exit 1
fi
grep -Fq 'HFOC integrated installer/dual-boot/management acceptance verification: OK' \
    <<<"$verifier_output"

expect_failure "missing integrated acceptance matrix" \
    bash scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh \
    "$TEST_DIR/missing.txt"

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^hfoc_integrated_installer_dual_boot_management_acceptance_version=1/\
hfoc_integrated_installer_dual_boot_management_acceptance_version=2/' "$MATRIX"
expect_failure "unsupported integrated acceptance version" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^target_release_id=HFOC-APPLIANCE-V1/\
target_release_id=HFOC-APPLIANCE-V2/' "$MATRIX"
expect_failure "integrated acceptance release identity mismatch" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_count=16/check_count=17/' "$MATRIX"
expect_failure "integrated acceptance undeclared check count" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_2_category=HARDWARE_DISCOVERY/\
check_2_category=ARCHITECTURE_AUDIT/' "$MATRIX"
expect_failure "reordered integrated acceptance category" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_2_id=HARDWARE_DISCOVERY-339/\
check_2_id=ARCHITECTURE_AUDIT-338/' "$MATRIX"
expect_failure "duplicate integrated acceptance check ID" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_2_name=hardware-os-discovery/\
check_2_name=installer-architecture-audit/' "$MATRIX"
expect_failure "duplicate integrated acceptance check name" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_2_target=test-installer-hardware-discovery-host/\
check_2_target=test-item-338-installer-architecture-audit-host/' "$MATRIX"
expect_failure "duplicate integrated acceptance check target" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_1_target=test-item-338-installer-architecture-audit-host/\
check_1_target=not-a-test/' "$MATRIX"
expect_failure "malformed integrated acceptance target" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
cat >>"$MATRIX" <<'EOF'
check_17_id=UNKNOWN-001
check_17_category=ARCHITECTURE_AUDIT
check_17_name=undeclared-check
check_17_target=test-item-338-installer-architecture-audit-host
check_17_marker=Item 338 installer architecture audit test: OK
EOF
expect_failure "undeclared integrated acceptance check" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_1_marker=Item 338 installer architecture audit test: OK/\
check_1_marker=Item 338 installer architecture audit test: OK!/' "$MATRIX"
expect_failure "malformed integrated acceptance marker" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_1_target=test-item-338-installer-architecture-audit-host/\
check_1_target=test-integrated-does-not-exist/' "$MATRIX"
expect_failure "missing integrated acceptance Makefile target" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
sed -i 's/^check_1_marker=Item 338 installer architecture audit test: OK/\
check_1_marker=Integrated acceptance proof marker missing 2026-09-06/' "$MATRIX"
expect_failure "missing integrated acceptance proof marker" run_verifier

cp release/hfoc-integrated-installer-dual-boot-management-acceptance.txt "$MATRIX"
if ! runner_output=$(run_runner); then
    echo 'integrated acceptance runner failed on canonical matrix' >&2
    exit 1
fi
grep -Fq 'HFOC integrated installer/dual-boot/management acceptance gate: OK' \
    <<<"$runner_output"

echo 'HFOC integrated installer/dual-boot/management acceptance host test: OK'
