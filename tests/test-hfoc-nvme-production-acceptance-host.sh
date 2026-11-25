#!/usr/bin/env bash
# Host regression harness for the ITEM-348 NVMe production acceptance gate.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
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

run_verifier() {
    bash scripts/verify-hfoc-nvme-production-acceptance.sh "$MATRIX"
}

run_runner() {
    HFOC_NVME_PRODUCTION_ACCEPTANCE_MATRIX="$MATRIX" \
        bash scripts/test-hfoc-nvme-production-acceptance.sh
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

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
if ! verifier_output=$(run_verifier); then
    echo 'canonical NVMe production acceptance verification failed' >&2
    exit 1
fi
grep -Fq 'HFOC NVMe production acceptance verification: OK' \
    <<<"$verifier_output"
grep -Fq 'hardware_status=NOT_TESTED' "$MATRIX"
grep -Fq 'supported_v1_namespace_count=1' "$MATRIX"
grep -Fq 'supported_v1_block_size=512' "$MATRIX"

expect_failure "missing NVMe production acceptance matrix" \
    bash scripts/verify-hfoc-nvme-production-acceptance.sh \
    "$TEST_DIR/missing.txt"

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^hfoc_nvme_production_acceptance_version=1/\
hfoc_nvme_production_acceptance_version=2/' "$MATRIX"
expect_failure "unsupported NVMe production acceptance version" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^target_release_id=HFOC-APPLIANCE-V1/\
target_release_id=HFOC-APPLIANCE-V2/' "$MATRIX"
expect_failure "NVMe production acceptance release identity mismatch" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^acceptance_class=SOFTWARE_PROOF/\
acceptance_class=PHYSICAL_ACCEPTANCE/' "$MATRIX"
expect_failure "software proof class replaced by physical acceptance" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^hardware_status=NOT_TESTED/\
hardware_status=PASS/' "$MATRIX"
expect_failure "unproven physical hardware marked PASS" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^supported_v1_namespace_count=1/\
supported_v1_namespace_count=2/' "$MATRIX"
expect_failure "multi-namespace policy outside ITEM-348" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^supported_v1_block_size=512/\
supported_v1_block_size=4096/' "$MATRIX"
expect_failure "unsupported V1 block-size evidence" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^gpt_mutation=NONE/\
gpt_mutation=ALLOWED/' "$MATRIX"
expect_failure "GPT mutation evidence violates read-only boundary" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^efi_nvram_mutation=NONE/\
efi_nvram_mutation=ALLOWED/' "$MATRIX"
expect_failure "EFI/NVRAM mutation evidence violates ITEM-348 boundary" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_count=3/check_count=4/' "$MATRIX"
expect_failure "NVMe production acceptance undeclared check count" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_2_category=NVME_STATE_INTEGRATION/\
check_2_category=NVME_HOST/' "$MATRIX"
expect_failure "reordered NVMe production acceptance category" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_2_id=NVME_STATE_INTEGRATION-348/\
check_2_id=NVME_HOST-348/' "$MATRIX"
expect_failure "duplicate NVMe production acceptance check ID" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_2_name=nvme-state-integration/\
check_2_name=nvme-host/' "$MATRIX"
expect_failure "duplicate NVMe production acceptance check name" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_2_target=test-nvme-state-integration-host/\
check_2_target=test-nvme-host/' "$MATRIX"
expect_failure "duplicate NVMe production acceptance check target" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
cat >>"$MATRIX" <<'EOF'
check_4_id=UNKNOWN-001
check_4_category=NVME_HOST
check_4_name=undeclared-check
check_4_target=test-nvme-host
check_4_marker=NVMe host tests: ALL OK
EOF
expect_failure "undeclared NVMe production acceptance check" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_1_target=test-nvme-host/\
check_1_target=not-a-test/' "$MATRIX"
expect_failure "malformed NVMe production acceptance target" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_1_marker=NVMe host tests: ALL OK/\
check_1_marker=NVMe host tests: ALL OK!/' "$MATRIX"
expect_failure "malformed NVMe production acceptance marker" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_1_target=test-nvme-host/\
check_1_target=test-nvme-production-does-not-exist/' "$MATRIX"
expect_failure "missing NVMe production acceptance Makefile target" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
sed -i 's/^check_1_marker=NVMe host tests: ALL OK/\
check_1_marker=NVMe production proof marker missing 2026-09-06/' "$MATRIX"
expect_failure "missing NVMe production acceptance proof marker" run_verifier

cp release/hfoc-nvme-production-acceptance.txt "$MATRIX"
if ! runner_output=$(run_runner); then
    echo 'NVMe production acceptance runner failed on canonical matrix' >&2
    exit 1
fi
grep -Fq 'HFOC NVMe production acceptance gate: OK' \
    <<<"$runner_output"

echo 'HFOC NVMe production acceptance host test: OK'
