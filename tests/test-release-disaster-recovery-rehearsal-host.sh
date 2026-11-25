#!/usr/bin/env bash
# Host regression harness for the appliance v1 disaster-recovery rehearsal.
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

marker=
case "$target" in
    test-fake-clean-image)
        marker='fake clean image ALL OK'
        ;;
    test-fake-restore)
        marker='fake restore ALL OK'
        ;;
    test-fake-service-recovery)
        marker='fake service recovery ALL OK'
        ;;
    test-fake-integration-handshake)
        marker='fake integration handshake ALL OK'
        ;;
    test-fake-support-evidence)
        marker='fake support evidence ALL OK'
        ;;
    test-fake-fail)
        printf 'fake failure\n' >&2
        exit 23
        ;;
    test-fake-no-marker)
        printf 'fake %s completed without marker\n' "$target"
        printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
        exit 0
        ;;
    *)
        printf 'unknown target: %s\n' "$target" >&2
        exit 64
        ;;
esac

printf '%s\n' "$marker"
printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
FAKE
chmod +x "$FAKE_MAKE"

write_matrix() {
    cat >"$FAKE_MATRIX"
}

write_full_matrix() {
    write_matrix <<'EOF'
release_disaster_recovery_rehearsal_version=1
target_release_id=HFOC-APPLIANCE-V1
check_count=5
check_1_id=CLEAN_IMAGE-001
check_1_category=CLEAN_IMAGE
check_1_name=clean-image-boot
check_1_target=test-fake-clean-image
check_1_marker=fake clean image ALL OK
check_2_id=RESTORE-001
check_2_category=RESTORE
check_2_name=backup-restore-recovery
check_2_target=test-fake-restore
check_2_marker=fake restore ALL OK
check_3_id=SERVICE_RECOVERY-001
check_3_category=SERVICE_RECOVERY
check_3_name=service-cascade-recovery
check_3_target=test-fake-service-recovery
check_3_marker=fake service recovery ALL OK
check_4_id=INTEGRATION_HANDSHAKE-001
check_4_category=INTEGRATION_HANDSHAKE
check_4_name=accounting-boundary-handshake
check_4_target=test-fake-integration-handshake
check_4_marker=fake integration handshake ALL OK
check_5_id=SUPPORT_BUNDLE_EVIDENCE-001
check_5_category=SUPPORT_BUNDLE_EVIDENCE
check_5_name=support-bundle-evidence
check_5_target=test-fake-support-evidence
check_5_marker=fake support evidence ALL OK
EOF
}

run_verifier() {
    bash scripts/verify-release-disaster-recovery-rehearsal.sh "$FAKE_MATRIX"
}

run_runner() {
    HFOC_RELEASE_DISASTER_RECOVERY_MATRIX="$FAKE_MATRIX" \
    HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_RELEASE_DISASTER_RECOVERY_ARTIFACT_DIR="$TEST_DIR/artifacts" \
        bash scripts/test-release-disaster-recovery-rehearsal.sh
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
    echo 'canonical disaster-recovery rehearsal verification failed' >&2
    exit 1
fi
grep -Fq 'Appliance v1 disaster-recovery rehearsal verification: OK' \
    <<<"$verifier_output"

expect_failure "missing disaster-recovery rehearsal matrix" \
    bash scripts/verify-release-disaster-recovery-rehearsal.sh \
    "$TEST_DIR/missing.txt"

write_full_matrix
sed -i 's/^release_disaster_recovery_rehearsal_version=1/\
release_disaster_recovery_rehearsal_version=2/' "$FAKE_MATRIX"
expect_failure "unsupported disaster-recovery rehearsal version" run_verifier

write_full_matrix
sed -i 's/^check_2_category=RESTORE/check_2_category=CLEAN_IMAGE/' \
    "$FAKE_MATRIX"
expect_failure "missing disaster-recovery rehearsal category" run_verifier

write_full_matrix
sed -i 's/^check_2_id=RESTORE-001/check_2_id=CLEAN_IMAGE-001/' \
    "$FAKE_MATRIX"
expect_failure "duplicate disaster-recovery rehearsal check ID" run_verifier

write_full_matrix
sed -i 's/^check_1_target=test-fake-clean-image/\
check_1_target=not-a-test/' "$FAKE_MATRIX"
expect_failure "malformed disaster-recovery rehearsal target" run_verifier

write_full_matrix
cat >>"$FAKE_MATRIX" <<'EOF'
check_6_id=UNKNOWN-001
check_6_category=CLEAN_IMAGE
check_6_name=undeclared-check
check_6_target=test-fake-clean-image
check_6_marker=fake clean image ALL OK
EOF
expect_failure "undeclared disaster-recovery rehearsal check" run_verifier

write_full_matrix
sed -i 's/^check_1_marker=fake clean image ALL OK/\
check_1_marker=fake clean image ALL OK!/' "$FAKE_MATRIX"
expect_failure "malformed disaster-recovery rehearsal marker" run_verifier

: >"$FAKE_STATE/events"
write_full_matrix
if ! runner_output=$(run_runner); then
    echo 'disaster-recovery rehearsal success case failed' >&2
    exit 1
fi
grep -Fq 'Appliance v1 disaster-recovery rehearsal: OK' \
    <<<"$runner_output"
for target in test-fake-clean-image test-fake-restore \
              test-fake-service-recovery test-fake-integration-handshake \
              test-fake-support-evidence; do
    grep -Fxq "start $target" "$FAKE_STATE/events"
    grep -Fxq "end $target" "$FAKE_STATE/events"
done
[[ "$(grep -c '^start ' "$FAKE_STATE/events")" == 5 ]] || {
    echo 'disaster-recovery rehearsal did not invoke exactly five stages' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fq 'PASS test-fake-clean-image' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'PASS test-fake-support-evidence' \
    "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'DISASTER RECOVERY REHEARSAL OK' \
    "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-support-evidence.log" ]] || {
    echo 'disaster-recovery rehearsal did not preserve all logs' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-restore/\
check_2_target=test-fake-fail/' "$FAKE_MATRIX"
set +e
HFOC_RELEASE_DISASTER_RECOVERY_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_RELEASE_DISASTER_RECOVERY_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-disaster-recovery-rehearsal.sh \
    >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'disaster-recovery rehearsal failure case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-clean-image' "$FAKE_STATE/events"
grep -Fxq 'start test-fake-fail' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-service-recovery' "$FAKE_STATE/events"; then
    echo 'disaster-recovery rehearsal started a queued stage after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
[[ -f "$TEST_DIR/artifacts/manifest.txt" ]] || {
    echo 'disaster-recovery rehearsal did not preserve failure manifest' >&2
    exit 1
}
grep -Fq 'FAIL test-fake-fail' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-fail.log" ]] || {
    echo 'disaster-recovery rehearsal did not preserve failure log' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-restore/\
check_2_target=test-fake-no-marker/' "$FAKE_MATRIX"
set +e
HFOC_RELEASE_DISASTER_RECOVERY_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_RELEASE_DISASTER_RECOVERY_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-disaster-recovery-rehearsal.sh \
    >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'disaster-recovery rehearsal missing-marker case unexpectedly succeeded' >&2
    exit 1
fi
if grep -Fxq 'start test-fake-service-recovery' "$FAKE_STATE/events"; then
    echo 'disaster-recovery rehearsal continued after missing marker' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
grep -Fq 'FAIL test-fake-no-marker' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'FAILURE missing-marker' "$TEST_DIR/artifacts/manifest.txt"

echo 'Appliance v1 disaster-recovery rehearsal host test: OK'
