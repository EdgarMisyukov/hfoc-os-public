#!/usr/bin/env bash
# Host regression harness for the ITEM-369 production end-to-end acceptance
# gate: canonical matrix, negative matrix fixtures, runner verdicts and the
# evidence-harvest path. It never runs a QEMU target.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX_SRC=release/hfoc-production-end-to-end-acceptance.txt
VERIFIER=scripts/verify-production-end-to-end-acceptance.sh
RUNNER=scripts/test-production-end-to-end-acceptance.sh
REPORT_VERIFIER=scripts/verify-production-end-to-end-acceptance-report.sh
DOC=docs/hfoc-production-end-to-end-acceptance.md
HARDWARE_REPORT=release/hfoc-hardware-acceptance-report.md

TEST_DIR=$(mktemp -d)
MATRIX=$TEST_DIR/matrix.txt
ARTIFACTS=$TEST_DIR/artifacts
HARVEST=$TEST_DIR/harvest
FAKE_MAKE=$TEST_DIR/fake-make

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$ARTIFACTS" "$HARVEST"

cat >"$FAKE_MAKE" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail

target=${1:-}
matrix=${HFOC_PRODUCTION_ACCEPTANCE_MATRIX:-release/hfoc-production-end-to-end-acceptance.txt}

count=$(sed -n 's/^stage_count=//p' "$matrix")
matched=0
for ((index = 1; index <= count; ++index)); do
    stage_target=$(sed -n "s/^stage_${index}_target=//p" "$matrix")
    if [[ "$stage_target" == "$target" ]]; then
        if [[ -n "${HFOC_FAKE_MAKE_FAIL_TARGET:-}" &&
              "$target" == "$HFOC_FAKE_MAKE_FAIL_TARGET" ]]; then
            echo "fake make: forced failure for $target" >&2
            exit 1
        fi
        # A target shared by several stages publishes every stage marker.
        sed -n "s/^stage_${index}_marker=//p" "$matrix"
        matched=1
    fi
done
if (( matched == 0 )); then
    echo "fake make: unknown target $target" >&2
    exit 1
fi
exit 0
EOF
chmod +x "$FAKE_MAKE"

fresh_matrix() {
    cp "$MATRIX_SRC" "$MATRIX"
}

run_verifier() {
    bash "$VERIFIER" "$MATRIX"
}

run_runner() {
    HFOC_PRODUCTION_ACCEPTANCE_MATRIX="$MATRIX" \
        HFOC_PRODUCTION_ACCEPTANCE_ARTIFACT_DIR="$ARTIFACTS" \
        HFOC_PRODUCTION_ACCEPTANCE_HARVEST_DIR="$HARVEST" \
        HFOC_MAKE_CMD="$FAKE_MAKE" \
        bash "$RUNNER"
}

run_report_verifier() {
    bash "$REPORT_VERIFIER" "$1" "$MATRIX"
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

fresh_matrix
if ! verifier_output=$(run_verifier); then
    echo 'canonical production end-to-end acceptance verification failed' >&2
    exit 1
fi
grep -Fq 'Production end-to-end acceptance verification: OK' \
    <<<"$verifier_output"

[[ -f "$DOC" ]] || {
    echo "missing production end-to-end acceptance doc" >&2
    exit 1
}
[[ -f "$HARDWARE_REPORT" ]] || {
    echo "missing hardware acceptance report" >&2
    exit 1
}
grep -Fq '**Status:** NOT TESTED' "$HARDWARE_REPORT"
grep -Fq 'stage_24_id=HARDWARE_ACCEPTANCE' "$MATRIX_SRC"
grep -Fq 'stage_24_state=NOT_TESTED' "$MATRIX_SRC"
grep -Fq 'stage_24_owner=ITEM-381' "$MATRIX_SRC"
grep -Fq 'stage_25_id=LICENSE_PAYLOAD' "$MATRIX_SRC"
grep -Fq 'stage_25_owner=ITEM-382' "$MATRIX_SRC"
grep -Fq 'stage_25_target=test-license-payload-host' "$MATRIX_SRC"
grep -Fq 'stage_13_target=test-production-recovery-reset-qemu' "$MATRIX_SRC"
grep -Fq 'stage_14_target=test-appliance-production-recovery-executor-host' \
    "$MATRIX_SRC"

expect_failure "missing production acceptance matrix" \
    bash "$VERIFIER" "$TEST_DIR/missing.txt"

fresh_matrix
sed -i 's/^hfoc_production_end_to_end_acceptance_version=2/\
hfoc_production_end_to_end_acceptance_version=3/' "$MATRIX"
expect_failure "unsupported production acceptance version" run_verifier

fresh_matrix
sed -i 's/^target_release_id=HFOC-APPLIANCE-V1/\
target_release_id=HFOC-APPLIANCE-V2/' "$MATRIX"
expect_failure "production acceptance release identity mismatch" run_verifier

fresh_matrix
sed -i 's/^stage_count=25/stage_count=26/' "$MATRIX"
expect_failure "production acceptance stage count mismatch" run_verifier

fresh_matrix
cat >>"$MATRIX" <<'EOF'
stage_26_id=UNDECLARED
stage_26_name=undeclared-stage
stage_26_owner=ITEM-000
stage_26_state=NOT_TESTED
stage_26_kind=none
stage_26_reason=undeclared fixture
EOF
expect_failure "undeclared production acceptance stage" run_verifier

fresh_matrix
sed -i 's/^stage_2_id=INSTALLER/stage_2_id=MEDIA_DIGEST/' "$MATRIX"
expect_failure "duplicate production acceptance stage id" run_verifier

fresh_matrix
sed -i 's/^stage_2_name=production-installer/\
stage_2_name=media-artifact-digest/' "$MATRIX"
expect_failure "duplicate production acceptance stage name" run_verifier

fresh_matrix
sed -i 's/^stage_1_state=PROVEN/stage_1_state=MAYBE/' "$MATRIX"
expect_failure "unknown production acceptance stage state" run_verifier

fresh_matrix
sed -i 's/^stage_1_kind=gate/stage_1_kind=none/' "$MATRIX"
expect_failure "proven stage without a runner kind" run_verifier

fresh_matrix
sed -i 's/^stage_1_target=test-release-artifact-host/\
stage_1_target=not-a-test/' "$MATRIX"
expect_failure "malformed production acceptance target" run_verifier

fresh_matrix
sed -i 's/^stage_1_target=test-release-artifact-host/\
stage_1_target=test-production-acceptance-missing-target/' "$MATRIX"
expect_failure "missing production acceptance Makefile target" run_verifier

fresh_matrix
sed -i 's/^stage_1_marker=Release artifact manifest verification: OK/\
stage_1_marker=Production acceptance proof marker missing 2026-09-11/' \
    "$MATRIX"
expect_failure "missing production acceptance proof marker" run_verifier

fresh_matrix
sed -i 's/^stage_1_marker=Release artifact manifest verification: OK/\
stage_1_marker=Release artifact manifest verification: OK!/' "$MATRIX"
expect_failure "malformed production acceptance marker" run_verifier

fresh_matrix
sed -i 's/^stage_24_kind=none/stage_24_kind=run/' "$MATRIX"
sed -i '/^stage_24_reason=/a\
stage_24_target=test-release-artifact-host' "$MATRIX"
expect_failure "not-tested stage carrying an execution target" run_verifier

fresh_matrix
sed -i 's/^stage_2_id=INSTALLER/stage_2_id=FIRST_BOOT/' "$MATRIX"
expect_failure "reordered production acceptance stages" run_verifier

fresh_matrix
sed -i 's/^stage_24_state=NOT_TESTED/stage_24_state=PROVEN/' "$MATRIX"
sed -i 's/^stage_24_kind=none/stage_24_kind=run/' "$MATRIX"
sed -i 's|^stage_24_reason=.*|stage_24_target=test-release-artifact-host|' \
    "$MATRIX"
sed -i '/^stage_24_target=test-release-artifact-host/a\
stage_24_marker=Release artifact manifest verification: OK' "$MATRIX"
expect_failure "hardware acceptance cannot be proven by QEMU" run_verifier

fresh_matrix
sed -i 's/^stage_19_state=DEGRADED/stage_19_state=NOT_TESTED/' "$MATRIX"
sed -i 's/^stage_19_kind=run/stage_19_kind=none/' "$MATRIX"
sed -i '/^stage_19_target=/d;/^stage_19_marker=/d' "$MATRIX"
sed -i 's/^stage_19_reason=.*/stage_19_reason=fixture skip/' "$MATRIX"
if ! grep -Fq '^stage_19_reason=' "$MATRIX"; then
    printf 'stage_19_reason=fixture skip\n' >>"$MATRIX"
fi
expect_failure "dependency stage silently skipped as not tested" run_verifier

fresh_matrix
sed -i 's/^stage_9_state=PROVEN/stage_9_state=NOT_TESTED/' "$MATRIX"
sed -i 's/^stage_9_kind=run/stage_9_kind=none/' "$MATRIX"
sed -i '/^stage_9_target=/d;/^stage_9_marker=/d' "$MATRIX"
printf 'stage_9_reason=fixture skip\n' >>"$MATRIX"
expect_failure "proven dependency stage silently skipped" run_verifier

fresh_matrix
sed -i '/^stage_21_reason=/d' "$MATRIX"
expect_failure "degraded stage without a machine-readable reason" run_verifier

fresh_matrix
if ! runner_output=$(run_runner); then
    echo 'production acceptance runner failed on the canonical matrix' >&2
    exit 1
fi
grep -Fq 'PRODUCTION END-TO-END ACCEPTANCE: ACCEPT' <<<"$runner_output"
REPORT=$ARTIFACTS/report.txt
[[ -f "$REPORT" ]] || {
    echo "production acceptance runner did not publish a report" >&2
    exit 1
}
grep -Fq 'gate_status=ACCEPT' "$REPORT"
grep -Fq 'hfoc_production_end_to_end_acceptance_report_version=2' "$REPORT"
grep -Eq '^run_started_utc=[0-9]{8}T[0-9]{6}Z$' "$REPORT"
grep -Eq '^run_finished_utc=[0-9]{8}T[0-9]{6}Z$' "$REPORT"
grep -Fq 'stage_13_status=PASS' "$REPORT"
grep -Fq 'stage_13_evidence=run' "$REPORT"
# ITEM-386: the service/HTTPS stage is PROVEN, so the runner reports it as
# expected=PROVEN with a PASS status and no degraded reason.  A PROVEN stage
# must never be rendered as DEGRADED here; the degradation contract itself
# stays covered by stage_22 below.
grep -Fq 'stage_7_expected=PROVEN' "$REPORT"
grep -Fq 'stage_7_status=PASS' "$REPORT"
if grep -Eq '^stage_7_status=DEGRADED$' "$REPORT"; then
    echo "stage_7 is DEGRADED while the matrix declares PROVEN" >&2
    exit 1
fi
if grep -Eq '^stage_7_reason=.+$' "$REPORT"; then
    echo "stage_7 carries a degraded reason while PROVEN" >&2
    exit 1
fi
grep -Fq 'stage_22_expected=DEGRADED' "$REPORT"
grep -Fq 'stage_22_status=DEGRADED' "$REPORT"
grep -Eq '^stage_22_reason=.+$' "$REPORT"
grep -Fq 'stage_24_expected=NOT_TESTED' "$REPORT"
grep -Fq 'stage_24_status=NOT_TESTED' "$REPORT"
grep -Fq 'stage_24_owner=ITEM-381' "$REPORT"
grep -Eq '^stage_24_reason=.+$' "$REPORT"
grep -Fq 'stage_25_expected=PROVEN' "$REPORT"
grep -Fq 'stage_25_status=PASS' "$REPORT"
grep -Fq 'stage_25_evidence=gate' "$REPORT"
grep -Fq 'stage_25_target=test-license-payload-host' "$REPORT"
if ! report_output=$(run_report_verifier "$REPORT"); then
    echo 'report verifier rejected the canonical report' >&2
    exit 1
fi
grep -Fq 'Production end-to-end acceptance report verification: OK' \
    <<<"$report_output"

rm -rf "${ARTIFACTS:?}"/*
rm -rf "${HARVEST:?}"/*
printf '%s PASS\n' test-production-installer-qemu \
    >"$HARVEST/suite-status.txt"
printf 'PASS %s\n' test-production-storage-boot-qemu \
    >"$HARVEST/manifest.txt"
if ! harvested_output=$(HFOC_FAKE_MAKE_FAIL_TARGET=test-production-installer-qemu \
        run_runner); then
    echo 'production acceptance runner failed on harvested evidence' >&2
    exit 1
fi
grep -Fq 'PRODUCTION END-TO-END ACCEPTANCE: ACCEPT' <<<"$harvested_output"
grep -Fq 'stage_2_evidence=harvest' "$ARTIFACTS/report.txt"
grep -Fq 'stage_3_evidence=harvest' "$ARTIFACTS/report.txt"

rm -rf "${ARTIFACTS:?}"/*
rm -rf "${HARVEST:?}"/*
if HFOC_FAKE_MAKE_FAIL_TARGET=test-production-installer-qemu \
        run_runner >/dev/null 2>&1; then
    echo 'production acceptance runner accepted a failing stage' >&2
    exit 1
fi
grep -Fq 'gate_status=REJECT' "$ARTIFACTS/report.txt"
grep -Fq 'stage_2_status=FAIL' "$ARTIFACTS/report.txt"
grep -Fq 'gate_reason=stage 2 INSTALLER failed' "$ARTIFACTS/report.txt"
grep -Fq 'stage_13_status=PASS' "$ARTIFACTS/report.txt"
if grep -Fq 'stage_13_status=UNOBSERVED' "$ARTIFACTS/report.txt"; then
    echo 'a later stage was skipped after an earlier failure' >&2
    exit 1
fi
if ! run_report_verifier "$ARTIFACTS/report.txt" >/dev/null 2>&1; then
    echo 'report verifier rejected a well-formed rejected report' >&2
    exit 1
fi

# Report verifier negative fixtures: a DEGRADED stage rendered as PASS, a
# missing gate reason and an ACCEPT gate over a failing stage.
# ITEM-386: stage_7 is PROVEN now, so the degraded-rendered-as-pass fixture
# uses stage_22 (still DEGRADED) to keep exercising the same rejection.  The
# rewrite is anchored on the observed status so it fails loudly if the report
# stops carrying a DEGRADED stage at all.
cp "$ARTIFACTS/report.txt" "$TEST_DIR/tampered-report.txt"
grep -Fq 'stage_22_status=DEGRADED' "$TEST_DIR/tampered-report.txt" || {
    echo 'report has no DEGRADED stage for the tamper fixture' >&2
    exit 1
}
sed -i 's/^stage_22_status=DEGRADED$/stage_22_status=PASS/' \
    "$TEST_DIR/tampered-report.txt"
expect_failure "degraded stage rendered as a pass" \
    run_report_verifier "$TEST_DIR/tampered-report.txt"

cp "$ARTIFACTS/report.txt" "$TEST_DIR/tampered-report.txt"
sed -i 's/^stage_2_status=FAIL$/stage_2_status=DEGRADED/' \
    "$TEST_DIR/tampered-report.txt"
sed -i 's/^gate_status=REJECT$/gate_status=ACCEPT/' \
    "$TEST_DIR/tampered-report.txt"
expect_failure "accept gate over a rewritten stage" \
    run_report_verifier "$TEST_DIR/tampered-report.txt"

cp "$ARTIFACTS/report.txt" "$TEST_DIR/tampered-report.txt"
sed -i '/^gate_reason=/d' "$TEST_DIR/tampered-report.txt"
expect_failure "report without a gate reason" \
    run_report_verifier "$TEST_DIR/tampered-report.txt"

cp "$ARTIFACTS/report.txt" "$TEST_DIR/tampered-report.txt"
sed -i '/^stage_13_status=/d' "$TEST_DIR/tampered-report.txt"
expect_failure "report missing a stage verdict" \
    run_report_verifier "$TEST_DIR/tampered-report.txt"

# ITEM-587: HTTPS_SERVICE is composed of two split parts.  The acceptance gate
# accepts only when the current run's harvest carries both as PASS; a failed or
# missing part rejects it even though the other part passed.
https_harvest() {
    rm -rf "${ARTIFACTS:?}"/*
    rm -rf "${HARVEST:?}"/*
    mkdir -p "$HARVEST"
    shift 0
}

https_harvest
printf '%s PASS\n' test-production-service-https-main-qemu \
    test-production-service-https-noentropy-qemu >"$HARVEST/suite-status.txt"
if ! https_output=$(HFOC_FAKE_MAKE_FAIL_TARGET=test-production-service-https-qemu \
        run_runner); then
    echo 'acceptance rejected a harvest with both HTTPS parts passed' >&2
    exit 1
fi
grep -Fq 'PRODUCTION END-TO-END ACCEPTANCE: ACCEPT' <<<"$https_output"
grep -Fq 'stage_7_status=PASS' "$ARTIFACTS/report.txt"
grep -Fq 'stage_7_evidence=harvest' "$ARTIFACTS/report.txt"

https_harvest
printf 'PASS %s\n' test-production-service-https-main-qemu \
    >"$HARVEST/suite-status.txt"
printf '%s FAIL\n' test-production-service-https-noentropy-qemu \
    >>"$HARVEST/suite-status.txt"
if HFOC_FAKE_MAKE_FAIL_TARGET=test-production-service-https-qemu \
        run_runner >/dev/null 2>&1; then
    echo 'acceptance accepted a harvest with a failed HTTPS part' >&2
    exit 1
fi
grep -Fq 'gate_status=REJECT' "$ARTIFACTS/report.txt"

https_harvest
printf '%s PASS\n' test-production-service-https-main-qemu \
    >"$HARVEST/suite-status.txt"
if HFOC_FAKE_MAKE_FAIL_TARGET=test-production-service-https-qemu \
        run_runner >/dev/null 2>&1; then
    echo 'acceptance accepted a harvest missing an HTTPS part' >&2
    exit 1
fi
grep -Fq 'gate_status=REJECT' "$ARTIFACTS/report.txt"

echo 'Production end-to-end acceptance host test: OK'
