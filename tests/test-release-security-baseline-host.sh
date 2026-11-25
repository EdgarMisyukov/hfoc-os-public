#!/usr/bin/env bash
# Host regression harness for the release security baseline runner.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
export FAKE_STATE="$TEST_DIR/state"
FAKE_MAKE=$TEST_DIR/fake-make
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
    test-fake-a|test-fake-b|test-fake-c|test-fake-d|test-fake-e|test-fake-f)
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

write_matrix() {
    cat >"$FAKE_MATRIX"
}

write_full_matrix() {
    write_matrix <<'EOF'
release_security_baseline_version=1
target_release_id=HFOC-APPLIANCE-V1
check_count=6
check_1_id=FAKE-001
check_1_category=WX
check_1_name=user-layout-and-mapping
check_1_target=test-fake-a
check_2_id=FAKE-002
check_2_category=SYSCALL_USER_POINTER
check_2_name=syscall-abi-user-pointer
check_2_target=test-fake-b
check_3_id=FAKE-003
check_3_category=LOCAL_CONTROL
check_3_name=local-control-outage-recovery
check_3_target=test-fake-c
check_4_id=FAKE-004
check_4_category=TRUST
check_4_name=trust-failure-denial
check_4_target=test-fake-d
check_5_id=FAKE-005
check_5_category=NETWORK_ADMISSION
check_5_name=network-admission-policy
check_5_target=test-fake-e
check_6_id=FAKE-006
check_6_category=INTEGRATION_DENIAL
check_6_name=integration-config-intent-denial
check_6_target=test-fake-f
EOF
}

run_verifier() {
    bash scripts/verify-release-security-baseline.sh "$FAKE_MATRIX"
}

run_runner() {
    HFOC_RELEASE_SECURITY_MATRIX="$FAKE_MATRIX" \
    HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_RELEASE_SECURITY_ARTIFACT_DIR="$TEST_DIR/artifacts" \
        bash scripts/test-release-security-baseline.sh
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
    echo 'canonical release security baseline verification failed' >&2
    exit 1
fi
grep -Fq 'Release security baseline verification: OK' \
    <<<"$verifier_output"

expect_failure "missing release security matrix" \
    bash scripts/verify-release-security-baseline.sh \
    "$TEST_DIR/missing.txt"

write_full_matrix
sed -i 's/^release_security_baseline_version=1/\
release_security_baseline_version=2/' "$FAKE_MATRIX"
expect_failure "unsupported release security baseline version" run_verifier

write_full_matrix
sed -i '/^check_2_category=/d' "$FAKE_MATRIX"
expect_failure "missing category coverage" run_verifier

write_full_matrix
sed -i 's/^check_2_id=FAKE-002/check_2_id=FAKE-001/' "$FAKE_MATRIX"
expect_failure "duplicate release security check ID" run_verifier

write_full_matrix
sed -i 's/^check_1_target=test-fake-a/check_1_target=not-a-test/' \
    "$FAKE_MATRIX"
expect_failure "malformed release security target" run_verifier

write_full_matrix
cat >>"$FAKE_MATRIX" <<'EOF'
check_7_id=FAKE-007
check_7_category=WX
check_7_name=undeclared-check
check_7_target=test-fake-a
EOF
expect_failure "undeclared release security check" run_verifier

: >"$FAKE_STATE/events"
write_full_matrix
if ! runner_output=$(run_runner); then
    echo 'release security baseline success case failed' >&2
    exit 1
fi
grep -Fq 'Release security baseline: OK' <<<"$runner_output"
for target in test-fake-a test-fake-b test-fake-c test-fake-d \
              test-fake-e test-fake-f; do
    grep -Fxq "start $target" "$FAKE_STATE/events"
    grep -Fxq "end $target" "$FAKE_STATE/events"
done
[[ "$(grep -c '^start ' "$FAKE_STATE/events")" == 6 ]] || {
    echo 'release security baseline did not invoke exactly six checks' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fq 'PASS test-fake-a' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'SECURITY BASELINE OK' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-f.log" ]] || {
    echo 'release security baseline did not preserve all logs' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-b/check_2_target=test-fake-fail/' \
    "$FAKE_MATRIX"
set +e
HFOC_RELEASE_SECURITY_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_RELEASE_SECURITY_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-security-baseline.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'release security baseline failure case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-a' "$FAKE_STATE/events"
grep -Fxq 'start test-fake-fail' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-c' "$FAKE_STATE/events"; then
    echo 'release security baseline started a queued check after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
[[ -f "$TEST_DIR/artifacts/manifest.txt" ]] || {
    echo 'release security baseline did not preserve failure manifest' >&2
    exit 1
}
grep -Fq 'FAIL test-fake-fail' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-fail.log" ]] || {
    echo 'release security baseline did not preserve failure log' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-b/check_2_target=test-fake-no-marker/' \
    "$FAKE_MATRIX"
set +e
HFOC_RELEASE_SECURITY_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_RELEASE_SECURITY_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-security-baseline.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'release security baseline missing marker case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-no-marker' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-c' "$FAKE_STATE/events"; then
    echo 'release security baseline accepted a missing ALL OK marker' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi

echo 'Release security baseline host test: OK'
