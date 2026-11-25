#!/usr/bin/env bash
# Host regression harness for the HFOC Appliance Platform release gate.
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

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"

run_verifier() {
    bash scripts/verify-hfoc-appliance-platform-release-acceptance.sh "$MATRIX"
}

run_runner() {
    HFOC_APPLIANCE_PLATFORM_RELEASE_ACCEPTANCE_MATRIX="$MATRIX" \
        bash scripts/test-hfoc-appliance-platform-release-acceptance.sh
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
    echo 'canonical platform release acceptance verification failed' >&2
    exit 1
fi
grep -Fq 'HFOC Appliance Platform release acceptance verification: OK' \
    <<<"$verifier_output"
grep -Fq 'check_count=12' \
    release/hfoc-appliance-platform-release-acceptance.txt
grep -Fq 'check_4_category=INSTALLER' \
    release/hfoc-appliance-platform-release-acceptance.txt
grep -Fq 'check_4_target=test-hfoc-integrated-installer-dual-boot-management-acceptance' \
    release/hfoc-appliance-platform-release-acceptance.txt

expect_failure "missing platform release acceptance matrix" \
    bash scripts/verify-hfoc-appliance-platform-release-acceptance.sh \
    "$TEST_DIR/missing.txt"

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^hfoc_appliance_platform_release_acceptance_version=1/\
hfoc_appliance_platform_release_acceptance_version=2/' "$MATRIX"
expect_failure "unsupported platform release acceptance version" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^target_release_id=HFOC-APPLIANCE-V1/\
target_release_id=HFOC-APPLIANCE-V2/' "$MATRIX"
expect_failure "platform release acceptance release identity mismatch" \
    run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_count=12/check_count=13/' "$MATRIX"
expect_failure "platform release acceptance undeclared check count" \
    run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_4_category=INSTALLER/\
check_4_category=MANAGEMENT/' "$MATRIX"
expect_failure "duplicate platform release acceptance installer category" \
    run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_2_category=PERSISTENCE/\
check_2_category=BOOT/' "$MATRIX"
expect_failure "reordered platform release acceptance category" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_2_id=PERSISTENCE-335/\
check_2_id=BOOT-334/' "$MATRIX"
expect_failure "duplicate platform release acceptance check ID" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_2_name=persistence-reboot/\
check_2_name=integrated-appliance-boot/' "$MATRIX"
expect_failure "duplicate platform release acceptance check name" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_2_target=test-persistent-config-reboot-acceptance-qemu/\
check_2_target=test-integrated-appliance-boot-acceptance-qemu/' "$MATRIX"
expect_failure "duplicate platform release acceptance check target" \
    run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_1_target=test-integrated-appliance-boot-acceptance-qemu/\
check_1_target=not-a-test/' "$MATRIX"
expect_failure "malformed platform release acceptance target" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
cat >>"$MATRIX" <<'EOF'
check_13_id=UNKNOWN-001
check_13_category=BOOT
check_13_name=undeclared-check
check_13_target=test-integrated-appliance-boot-acceptance-qemu
check_13_marker=Integrated appliance boot acceptance: OK
EOF
expect_failure "undeclared platform release acceptance check" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_1_marker=Integrated appliance boot acceptance: OK/\
check_1_marker=Integrated appliance boot acceptance: OK!/' "$MATRIX"
expect_failure "malformed platform release acceptance marker" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_1_target=test-integrated-appliance-boot-acceptance-qemu/\
check_1_target=test-platform-release-does-not-exist/' "$MATRIX"
expect_failure "missing platform release acceptance Makefile target" \
    run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
sed -i 's/^check_1_marker=Integrated appliance boot acceptance: OK/\
check_1_marker=Platform release proof marker missing 2026-09-06/' "$MATRIX"
expect_failure "missing platform release acceptance proof marker" run_verifier

cp release/hfoc-appliance-platform-release-acceptance.txt "$MATRIX"
if ! runner_output=$(run_runner); then
    echo 'platform release acceptance runner failed on canonical matrix' >&2
    exit 1
fi
grep -Fq 'HFOC Appliance Platform release acceptance gate: OK' \
    <<<"$runner_output"

echo 'HFOC Appliance Platform release acceptance host test: OK'
