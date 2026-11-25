#!/usr/bin/env bash
# Host regression harness for the appliance v1 acceptance gate.
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
marker=
printf 'start %s\n' "$target" >>"$FAKE_STATE/events"

case "$target" in
    test-fake-release-profile)
        marker='fake release profile ALL OK'
        ;;
    test-fake-build-input)
        marker='fake build input OK'
        ;;
    test-fake-reproducible)
        marker='fake reproducible comparison OK'
        ;;
    test-fake-artifact)
        marker='fake artifact manifest OK'
        ;;
    test-fake-upgrade)
        marker='fake upgrade matrix OK'
        ;;
    test-fake-backup)
        marker='fake backup restore matrix OK'
        ;;
    test-fake-security)
        marker='fake security baseline OK'
        ;;
    test-fake-operational)
        marker='fake operational baseline OK'
        ;;
    test-fake-disaster)
        marker='fake disaster recovery OK'
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
appliance_v1_acceptance_version=1
target_release_id=HFOC-APPLIANCE-V1
check_count=9
check_1_id=RELEASE_PROFILE-001
check_1_category=RELEASE_PROFILE
check_1_name=release-profile-descriptor
check_1_target=test-fake-release-profile
check_1_marker=fake release profile ALL OK
check_2_id=BUILD_INPUT-001
check_2_category=BUILD_INPUT
check_2_name=build-input-manifest
check_2_target=test-fake-build-input
check_2_marker=fake build input OK
check_3_id=REPRODUCIBILITY-001
check_3_category=REPRODUCIBILITY
check_3_name=reproducible-image-comparison
check_3_target=test-fake-reproducible
check_3_marker=fake reproducible comparison OK
check_4_id=ARTIFACT_MANIFEST-001
check_4_category=ARTIFACT_MANIFEST
check_4_name=release-artifact-and-license-manifest
check_4_target=test-fake-artifact
check_4_marker=fake artifact manifest OK
check_5_id=UPGRADE_MATRIX-001
check_5_category=UPGRADE_MATRIX
check_5_name=release-upgrade-matrix
check_5_target=test-fake-upgrade
check_5_marker=fake upgrade matrix OK
check_6_id=BACKUP_RESTORE_MATRIX-001
check_6_category=BACKUP_RESTORE_MATRIX
check_6_name=release-backup-restore-matrix
check_6_target=test-fake-backup
check_6_marker=fake backup restore matrix OK
check_7_id=SECURITY_BASELINE-001
check_7_category=SECURITY_BASELINE
check_7_name=release-security-baseline
check_7_target=test-fake-security
check_7_marker=fake security baseline OK
check_8_id=OPERATIONAL_BASELINE-001
check_8_category=OPERATIONAL_BASELINE
check_8_name=release-operational-baseline
check_8_target=test-fake-operational
check_8_marker=fake operational baseline OK
check_9_id=DISASTER_RECOVERY-001
check_9_category=DISASTER_RECOVERY
check_9_name=disaster-recovery-rehearsal
check_9_target=test-fake-disaster
check_9_marker=fake disaster recovery OK
EOF
}

run_verifier() {
    bash scripts/verify-release-appliance-v1-acceptance.sh "$FAKE_MATRIX"
}

run_runner() {
    HFOC_APPLIANCE_V1_ACCEPTANCE_MATRIX="$FAKE_MATRIX" \
    HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_APPLIANCE_V1_ACCEPTANCE_ARTIFACT_DIR="$TEST_DIR/artifacts" \
        bash scripts/test-release-appliance-v1-acceptance.sh
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
grep -Fq 'check_4_name=release-artifact-and-license-manifest' "$FAKE_MATRIX"
if ! verifier_output=$(run_verifier); then
    echo 'canonical appliance v1 acceptance verification failed' >&2
    exit 1
fi
grep -Fq 'Appliance v1 acceptance gate verification: OK' \
    <<<"$verifier_output"

expect_failure "missing appliance v1 acceptance matrix" \
    bash scripts/verify-release-appliance-v1-acceptance.sh \
    "$TEST_DIR/missing.txt"

write_full_matrix
sed -i 's/^appliance_v1_acceptance_version=1/\
appliance_v1_acceptance_version=2/' "$FAKE_MATRIX"
expect_failure "unsupported appliance v1 acceptance version" run_verifier

write_full_matrix
sed -i 's/^check_2_category=BUILD_INPUT/\
check_2_category=RELEASE_PROFILE/' "$FAKE_MATRIX"
expect_failure "missing appliance v1 acceptance category" run_verifier

write_full_matrix
sed -i 's/^check_2_id=BUILD_INPUT-001/\
check_2_id=RELEASE_PROFILE-001/' "$FAKE_MATRIX"
expect_failure "duplicate appliance v1 acceptance check ID" run_verifier

write_full_matrix
sed -i 's/^check_1_target=test-fake-release-profile/\
check_1_target=not-a-test/' "$FAKE_MATRIX"
expect_failure "malformed appliance v1 acceptance target" run_verifier

write_full_matrix
cat >>"$FAKE_MATRIX" <<'EOF'
check_10_id=UNKNOWN-001
check_10_category=RELEASE_PROFILE
check_10_name=undeclared-check
check_10_target=test-fake-release-profile
check_10_marker=fake release profile ALL OK
EOF
expect_failure "undeclared appliance v1 acceptance check" run_verifier

write_full_matrix
sed -i 's/^check_1_marker=fake release profile ALL OK/\
check_1_marker=fake release profile ALL OK!/' "$FAKE_MATRIX"
expect_failure "malformed appliance v1 acceptance marker" run_verifier

: >"$FAKE_STATE/events"
write_full_matrix
if ! runner_output=$(run_runner); then
    echo 'appliance v1 acceptance success case failed' >&2
    exit 1
fi
grep -Fq 'Appliance v1 acceptance gate: ACCEPT' <<<"$runner_output"
for target in test-fake-release-profile test-fake-build-input \
              test-fake-reproducible test-fake-artifact test-fake-upgrade \
              test-fake-backup test-fake-security test-fake-operational \
              test-fake-disaster; do
    grep -Fxq "start $target" "$FAKE_STATE/events"
    grep -Fxq "end $target" "$FAKE_STATE/events"
done
[[ "$(grep -c '^start ' "$FAKE_STATE/events")" == 9 ]] || {
    echo 'appliance v1 acceptance did not invoke exactly nine stages' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fq 'PASS test-fake-release-profile' \
    "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'PASS test-fake-disaster' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'APPLIANCE V1 ACCEPTANCE OK' \
    "$TEST_DIR/artifacts/manifest.txt"

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-build-input/\
check_2_target=test-fake-fail/' "$FAKE_MATRIX"
set +e
HFOC_APPLIANCE_V1_ACCEPTANCE_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_APPLIANCE_V1_ACCEPTANCE_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-appliance-v1-acceptance.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'appliance v1 acceptance failure case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-release-profile' "$FAKE_STATE/events"
grep -Fxq 'start test-fake-fail' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-reproducible' "$FAKE_STATE/events"; then
    echo 'appliance v1 acceptance started a queued stage after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
[[ -f "$TEST_DIR/artifacts/manifest.txt" ]] || {
    echo 'appliance v1 acceptance did not preserve failure manifest' >&2
    exit 1
}
grep -Fq 'FAIL test-fake-fail' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-fail.log" ]] || {
    echo 'appliance v1 acceptance did not preserve failure log' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-build-input/\
check_2_target=test-fake-no-marker/' "$FAKE_MATRIX"
set +e
HFOC_APPLIANCE_V1_ACCEPTANCE_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_APPLIANCE_V1_ACCEPTANCE_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-release-appliance-v1-acceptance.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'appliance v1 acceptance missing-marker case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-no-marker' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-reproducible' "$FAKE_STATE/events"; then
    echo 'appliance v1 acceptance continued after missing marker' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
grep -Fq 'FAIL test-fake-no-marker' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'FAILURE missing-marker' "$TEST_DIR/artifacts/manifest.txt"

echo 'Appliance v1 acceptance gate host test: OK'
