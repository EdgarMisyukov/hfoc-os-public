#!/usr/bin/env bash
# Host regression harness for the release operational baseline runner.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
export FAKE_STATE="$TEST_DIR/state"
FAKE_MAKE=$TEST_DIR/fake-make
FAKE_QEMU_MAKE=$TEST_DIR/fake-qemu-make
FAKE_MATRIX=$TEST_DIR/matrix.txt
mkdir -p "$FAKE_STATE"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cat >"$FAKE_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

target=${1:-}
printf 'start %s\n' "$target" >>"$FAKE_STATE/events"

case "$target" in
    test-fake-boot|test-fake-state-schema|test-fake-state-migration|\
    test-fake-service-dependency|test-fake-service-supervisor|\
    test-fake-storage-health|test-fake-service-health|\
    test-fake-support-manifest|test-fake-support-export|\
    test-fake-candidate-preflight|test-fake-update-failure|\
    test-fake-recovery-image|test-fake-power-cycle|test-fake-qemu)
        printf 'fake %s ALL OK\n' "$target"
        printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
        ;;
    test-fake-fail)
        printf 'fake failure\n' >&2
        exit 23
        ;;
    test-fake-no-marker)
        printf 'fake %s completed without marker\n' "$target"
        printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
        ;;
    *)
        printf 'unknown target: %s\n' "$target" >&2
        exit 64
        ;;
esac
FAKE
chmod +x "$FAKE_MAKE"

cat >"$FAKE_QEMU_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

case "${1:-}" in
    test-boot-policy-regression)
        printf 'QEMU boot policy regression test: OK\n'
        ;;
    test-boot-policy-fail)
        printf 'proof failed\n' >&2
        exit 23
        ;;
    test-boot-policy-no-marker)
        printf 'proof completed\n'
        ;;
    *)
        printf 'unknown qemu target: %s\n' "${1:-}" >&2
        exit 64
        ;;
esac
FAKE
chmod +x "$FAKE_QEMU_MAKE"

write_qemu_missing_fake() {
    cat >"$FAKE_QEMU_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

case "${1:-}" in
    test-boot-policy-regression)
        printf 'proof completed\n'
        ;;
    *)
        printf 'unknown qemu target: %s\n' "${1:-}" >&2
        exit 64
        ;;
esac
FAKE
    chmod +x "$FAKE_QEMU_MAKE"
}

write_qemu_fail_fake() {
    cat >"$FAKE_QEMU_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

case "${1:-}" in
    test-boot-policy-regression)
        printf 'proof failed\n' >&2
        exit 23
        ;;
    *)
        printf 'unknown qemu target: %s\n' "${1:-}" >&2
        exit 64
        ;;
esac
FAKE
    chmod +x "$FAKE_QEMU_MAKE"
}

write_matrix() {
    cat >"$FAKE_MATRIX"
}

write_full_matrix() {
    write_matrix <<'EOF'
release_operational_baseline_version=1
target_release_id=HFOC-APPLIANCE-V1
check_count=14
check_1_id=BOOT-001
check_1_category=BOOT
check_1_name=boot-policy-persistence
check_1_target=test-fake-boot
check_2_id=STATE-001
check_2_category=STATE
check_2_name=state-schema-registry
check_2_target=test-fake-state-schema
check_3_id=STATE-002
check_3_category=STATE
check_3_name=state-migration-transaction
check_3_target=test-fake-state-migration
check_4_id=SERVICES-001
check_4_category=SERVICES
check_4_name=service-dependency-validation
check_4_target=test-fake-service-dependency
check_5_id=SERVICES-002
check_5_category=SERVICES
check_5_name=service-supervisor-lifecycle
check_5_target=test-fake-service-supervisor
check_6_id=HEALTH-001
check_6_category=HEALTH
check_6_name=storage-health-contract
check_6_target=test-fake-storage-health
check_7_id=HEALTH-002
check_7_category=HEALTH
check_7_name=service-health-aggregation
check_7_target=test-fake-service-health
check_8_id=SUPPORT_BUNDLE-001
check_8_category=SUPPORT_BUNDLE
check_8_name=support-bundle-manifest
check_8_target=test-fake-support-manifest
check_9_id=SUPPORT_BUNDLE-002
check_9_category=SUPPORT_BUNDLE
check_9_name=support-bundle-export
check_9_target=test-fake-support-export
check_10_id=UPDATE-001
check_10_category=UPDATE
check_10_name=candidate-preflight-boundary
check_10_target=test-fake-candidate-preflight
check_11_id=UPDATE-002
check_11_category=UPDATE
check_11_name=update-failure-recovery
check_11_target=test-fake-update-failure
check_12_id=RECOVERY-001
check_12_category=RECOVERY
check_12_name=recovery-image-selection
check_12_target=test-fake-recovery-image
check_13_id=RECOVERY-002
check_13_category=RECOVERY
check_13_name=power-cycle-continuity
check_13_target=test-fake-power-cycle
check_14_id=QEMU_LIVENESS-001
check_14_category=QEMU_LIVENESS
check_14_name=qemu-liveness
check_14_target=test-fake-qemu
EOF
}

run_verifier() {
    bash scripts/verify-release-operational-baseline.sh "$FAKE_MATRIX"
}

run_runner() {
    HFOC_RELEASE_OPERATIONAL_MATRIX="$FAKE_MATRIX" \
    HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_RELEASE_OPERATIONAL_ARTIFACT_DIR="$TEST_DIR/artifacts" \
        bash scripts/test-release-operational-baseline.sh
}

run_qemu_wrapper() {
    HFOC_OPERATIONAL_QEMU_MAKE_CMD="$FAKE_QEMU_MAKE" \
        bash scripts/test-release-operational-qemu-liveness.sh
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

write_full_matrix
if ! verifier_output=$(run_verifier); then
    echo 'canonical release operational baseline verification failed' >&2
    exit 1
fi
grep -Fq 'Release operational baseline verification: OK' \
    <<<"$verifier_output"

expect_failure "missing release operational matrix" \
    bash scripts/verify-release-operational-baseline.sh \
    "$TEST_DIR/missing.txt"

write_full_matrix
sed -i 's/^release_operational_baseline_version=1/\
release_operational_baseline_version=2/' "$FAKE_MATRIX"
expect_failure "unsupported release operational baseline version" run_verifier

write_full_matrix
sed -i '/^check_2_category=/d' "$FAKE_MATRIX"
expect_failure "missing category coverage" run_verifier

write_full_matrix
sed -i 's/^check_2_id=STATE-001/check_2_id=BOOT-001/' "$FAKE_MATRIX"
expect_failure "duplicate release operational check ID" run_verifier

write_full_matrix
sed -i 's/^check_1_target=test-fake-boot/check_1_target=not-a-test/' \
    "$FAKE_MATRIX"
expect_failure "malformed release operational target" run_verifier

write_full_matrix
cat >>"$FAKE_MATRIX" <<'EOF'
check_15_id=BOOT-002
check_15_category=BOOT
check_15_name=undeclared-check
check_15_target=test-fake-boot
EOF
expect_failure "undeclared release operational check" run_verifier

: >"$FAKE_STATE/events"
write_full_matrix
if ! runner_output=$(run_runner); then
    echo 'release operational baseline success case failed' >&2
    exit 1
fi
grep -Fq 'Release operational baseline: OK' <<<"$runner_output"
for target in test-fake-boot test-fake-state-schema test-fake-state-migration \
              test-fake-service-dependency test-fake-service-supervisor \
              test-fake-storage-health test-fake-service-health \
              test-fake-support-manifest test-fake-support-export \
              test-fake-candidate-preflight test-fake-update-failure \
              test-fake-recovery-image test-fake-power-cycle test-fake-qemu; do
    grep -Fxq "start $target" "$FAKE_STATE/events"
    grep -Fxq "end $target" "$FAKE_STATE/events"
done
[[ "$(grep -c '^start ' "$FAKE_STATE/events")" == 14 ]] || {
    echo 'release operational baseline did not invoke exactly fourteen checks' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fq 'PASS test-fake-boot' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'OPERATIONAL BASELINE OK' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-qemu.log" ]] || {
    echo 'release operational baseline did not preserve all logs' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-state-schema$/check_2_target=test-fake-fail/' "$FAKE_MATRIX"
set +e
HFOC_RELEASE_OPERATIONAL_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_RELEASE_OPERATIONAL_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-operational-baseline.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'release operational baseline failure case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-boot' "$FAKE_STATE/events"
grep -Fxq 'start test-fake-fail' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-state-migration' "$FAKE_STATE/events"; then
    echo 'release operational baseline started a queued check after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
[[ -f "$TEST_DIR/artifacts/manifest.txt" ]] || {
    echo 'release operational baseline did not preserve failure manifest' >&2
    exit 1
}
grep -Fq 'FAIL test-fake-fail' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-fail.log" ]] || {
    echo 'release operational baseline did not preserve failure log' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-state-schema$/check_2_target=test-fake-no-marker/' "$FAKE_MATRIX"
set +e
HFOC_RELEASE_OPERATIONAL_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_RELEASE_OPERATIONAL_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-operational-baseline.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'release operational baseline missing marker case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-no-marker' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-state-migration' "$FAKE_STATE/events"; then
    echo 'release operational baseline accepted a missing ALL OK marker' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi

if ! qemu_output=$(run_qemu_wrapper); then
    echo 'release operational QEMU liveness success case failed' >&2
    exit 1
fi
grep -Fq 'Release operational QEMU liveness: ALL OK' <<<"$qemu_output"

write_qemu_missing_fake
expect_failure "release operational QEMU liveness missing marker" \
    env HFOC_OPERATIONAL_QEMU_MAKE_CMD="$FAKE_QEMU_MAKE" \
    bash scripts/test-release-operational-qemu-liveness.sh

write_qemu_fail_fake
expect_failure "release operational QEMU liveness failure status" \
    env HFOC_OPERATIONAL_QEMU_MAKE_CMD="$FAKE_QEMU_MAKE" \
    bash scripts/test-release-operational-qemu-liveness.sh

echo 'Release operational baseline host test: OK'
