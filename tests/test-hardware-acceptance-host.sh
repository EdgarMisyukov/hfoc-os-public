#!/usr/bin/env bash
# ITEM-381 focused host proof for the hardware acceptance harness, matrix and
# report. The fixture never touches /dev/sda: it drives the harness through a
# fake sysfs root and recorded fake writers, so a guard bypass or a device
# write would be observed as an extra invocation.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

HARNESS=scripts/hfoc-hardware-acceptance.sh
MATRIX_VERIFIER=scripts/verify-hardware-acceptance-matrix.sh
REPORT_VERIFIER=scripts/verify-hardware-acceptance-report.sh
MATRIX_SRC=release/hfoc-hardware-acceptance-matrix.txt
PRODUCTION_MATRIX=release/hfoc-production-end-to-end-acceptance.txt
HARDWARE_REPORT=release/hfoc-hardware-acceptance-report.md

TEST_DIR=$(mktemp -d)
SYSFS=$TEST_DIR/sysfs
FAKE_BIN=$TEST_DIR/bin
WRITER_LOG=$TEST_DIR/writers.log
RUN_DIR=$TEST_DIR/run
MANIFEST=$TEST_DIR/manifest.txt

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$SYSFS/block/sda/device" "$FAKE_BIN" "$RUN_DIR"

write_identity() {
    local model=$1
    local serial=$2
    local root=$3

    printf '%s\n' "$model" >"$SYSFS/block/sda/device/model"
    cat >"$FAKE_BIN/lsblk" <<EOF
#!/usr/bin/env bash
printf '%s\n' '$serial'
EOF
    cat >"$FAKE_BIN/findmnt" <<EOF
#!/usr/bin/env bash
printf '%s\n' '$root'
EOF
    chmod +x "$FAKE_BIN/lsblk" "$FAKE_BIN/findmnt"
}

write_identity 'HFOC Example SATA Fixture' 'DEADBEEF00000001' '/dev/nvme0n1p2'

# Every writer the acceptance path must never reach is recorded. The harness is
# not expected to call any of them; the fixture fails when one appears.
for writer in dd efibootmgr systemctl partx sgdisk wipefs mkfs.ext4; do
    cat >"$FAKE_BIN/$writer" <<EOF
#!/usr/bin/env bash
printf '%s %s\n' '$writer' "\$*" >>'$WRITER_LOG'
exit 0
EOF
    chmod +x "$FAKE_BIN/$writer"
done

export PATH="$FAKE_BIN:$PATH"
export HFOC_HARDWARE_ACCEPTANCE_SYSFS_ROOT="$SYSFS"
export HFOC_HARDWARE_ACCEPTANCE_RUN_DIR="$RUN_DIR"
export HFOC_HARDWARE_ACCEPTANCE_MANIFEST="$MANIFEST"

writer_invocations() {
    if [[ -f "$WRITER_LOG" ]]; then
        wc -l <"$WRITER_LOG"
    else
        printf '0\n'
    fi
}

require_no_writers() {
    local label=$1
    if [[ "$(writer_invocations)" != "0" ]]; then
        echo "$label reached a disk writer" >&2
        cat "$WRITER_LOG" >&2
        exit 1
    fi
}

expect_status() {
    local expected=$1
    local label=$2
    shift 2

    local status=0
    set +e
    "$@" >/dev/null 2>&1
    status=$?
    set -e
    if [[ "$status" != "$expected" ]]; then
        echo "$label exited ${status}, expected ${expected}" >&2
        exit 1
    fi
}

echo '== matrix verifier =='
bash "$MATRIX_VERIFIER" "$MATRIX_SRC" >/dev/null
require_no_writers 'matrix verification'

expect_status 1 'matrix with a wrong version' \
    env -u HFOC_HARDWARE_ACCEPTANCE_SYSFS_ROOT \
    bash -c "sed 's/^hfoc_hardware_acceptance_matrix_version=1/\
hfoc_hardware_acceptance_matrix_version=2/' '$MATRIX_SRC' \
        >'$TEST_DIR/m1.txt'; bash '$MATRIX_VERIFIER' '$TEST_DIR/m1.txt'"

expect_status 1 'matrix without a hardware observation' \
    bash -c "sed 's/^stage_3_hardware_observation=.*/\
stage_3_hardware_observation=/' '$MATRIX_SRC' >'$TEST_DIR/m2.txt'; \
        bash '$MATRIX_VERIFIER' '$TEST_DIR/m2.txt'"

expect_status 1 'matrix with an unknown software target' \
    bash -c "sed 's/^stage_3_software_target=.*/\
stage_3_software_target=not-a-test/' '$MATRIX_SRC' >'$TEST_DIR/m3.txt'; \
        bash '$MATRIX_VERIFIER' '$TEST_DIR/m3.txt'"

expect_status 1 'matrix that drifts from the production stage order' \
    bash -c "sed 's/^stage_3_id=STORAGE_STATE/stage_3_id=FIRST_BOOT/' \
        '$MATRIX_SRC' >'$TEST_DIR/m4.txt'; \
        bash '$MATRIX_VERIFIER' '$TEST_DIR/m4.txt'"

expect_status 1 'matrix missing an implemented production stage' \
    bash -c "sed '/^stage_25_/d' '$MATRIX_SRC' \
        | sed 's/^stage_count=25/stage_count=24/' >'$TEST_DIR/m5.txt'; \
        bash '$MATRIX_VERIFIER' '$TEST_DIR/m5.txt'"

expect_status 1 'matrix whose AUTH stage lost the enrollment steps' \
    bash -c "sed 's/^stage_8_hardware_observation=.*/\
stage_8_hardware_observation=an unauthenticated request is denied/' \
        '$MATRIX_SRC' >'$TEST_DIR/m6.txt'; \
        bash '$MATRIX_VERIFIER' '$TEST_DIR/m6.txt'"

expect_status 1 'matrix whose MANAGEMENT stage lost the authenticated targets' \
    bash -c "sed 's#^stage_9_hardware_observation=.*#\
stage_9_hardware_observation=the read-only management plane serves the state#' \
        '$MATRIX_SRC' >'$TEST_DIR/m7.txt'; \
        bash '$MATRIX_VERIFIER' '$TEST_DIR/m7.txt'"

echo '== identity guard =='
bash "$HARNESS" --guard >/dev/null
require_no_writers 'matching identity guard'

write_identity 'HFOC Example SATA Fixture' 'DEADBEEFDEADBEEF' '/dev/nvme0n1p2'
expect_status 90 'serial mismatch guard' bash "$HARNESS" --guard
require_no_writers 'serial mismatch guard'

write_identity 'SAMSUNG MZ7LN256' 'DEADBEEF00000001' '/dev/nvme0n1p2'
expect_status 90 'model mismatch guard' bash "$HARNESS" --guard
require_no_writers 'model mismatch guard'

write_identity 'HFOC Example SATA Fixture' 'DEADBEEF00000001' '/dev/sda1'
expect_status 90 'root mismatch guard' bash "$HARNESS" --guard
require_no_writers 'root mismatch guard'

write_identity 'HFOC Example SATA Fixture' 'DEADBEEF00000001' '/dev/nvme0n1p2'

echo '== arming refuses to run unattended =='
expect_status 1 'arm without a confirmation token' \
    env -u CI -u HFOC_HARDWARE_ACCEPTANCE_CONFIRM bash "$HARNESS" --arm
expect_status 1 'arm inside CI' \
    env CI=true HFOC_HARDWARE_ACCEPTANCE_CONFIRM=ITEM-381 \
    bash "$HARNESS" --arm
require_no_writers 'refused arming'

echo '== arming the human-triggered run =='
printf 'hfoc fixture image\n' >"$TEST_DIR/fixture.iso"
arm_status=0
set +e
CI= \
HFOC_HARDWARE_ACCEPTANCE_CONFIRM=ITEM-381 \
HFOC_HARDWARE_ACCEPTANCE_INITIATOR='fixture-operator' \
HFOC_HARDWARE_ACCEPTANCE_IMAGE="$TEST_DIR/fixture.iso" \
    bash "$HARNESS" --arm >"$TEST_DIR/arm.out" 2>&1
arm_status=$?
set -e
if [[ "$arm_status" != "92" ]]; then
    echo "arm exited ${arm_status}, expected 92 (awaiting human run)" >&2
    cat "$TEST_DIR/arm.out" >&2
    exit 1
fi
require_no_writers 'armed run'
[[ -f "$MANIFEST" ]] || {
    echo 'arming did not write a run manifest' >&2
    exit 1
}
AUDIT_LINE=$(sed -n 's/^audit_line=//p' "$MANIFEST")
[[ -n "$AUDIT_LINE" ]] || {
    echo 'the armed manifest carries no audit line' >&2
    exit 1
}
for fragment in 'initiator=' 'started_utc=' 'device=/dev/sda' 'model=' \
                'serial=' 'declared_model=' 'declared_serial=' \
                'declared_root=' 'image_sha256='; do
    if [[ "$AUDIT_LINE" != *"$fragment"* ]]; then
        echo "the audit line is missing ${fragment}" >&2
        cat "$TEST_DIR/arm.out" >&2
        exit 1
    fi
done

echo '== recording observations and rendering the report =='
expect_status 1 'recording a PASS without evidence' \
    bash "$HARNESS" --record MEDIA_DIGEST PASS ''
expect_status 1 'recording an unknown stage' \
    bash "$HARNESS" --record NOT_A_STAGE PASS 'evidence'

bash "$HARNESS" --record MEDIA_DIGEST PASS \
    'host fixture observation' >/dev/null

report_status=0
set +e
bash "$HARNESS" --report >"$TEST_DIR/report.out" 2>&1
report_status=$?
set -e
if [[ "$report_status" != "0" ]]; then
    echo "report exited ${report_status}, expected 0" >&2
    cat "$TEST_DIR/report.out" >&2
    exit 1
fi
REPORT=$RUN_DIR/report.txt
[[ -f "$REPORT" ]] || {
    echo 'the harness did not publish a report' >&2
    exit 1
}
grep -Fq 'hfoc_hardware_acceptance_report_version=1' "$REPORT"
grep -Fq 'acceptance_device=/dev/sda' "$REPORT"
grep -Fq 'stage_1_status=PASS' "$REPORT"
grep -Fq 'stage_1_evidence=hardware' "$REPORT"
grep -Fq 'stage_1_hardware_evidence=host fixture observation' "$REPORT"
grep -Fq 'stage_2_status=NOT_TESTED' "$REPORT"
grep -Fq 'gate_status=INCOMPLETE' "$REPORT"
grep -Fq 'physical_run=NO' "$REPORT"
bash "$REPORT_VERIFIER" "$REPORT" "$MATRIX_SRC" >/dev/null
require_no_writers 'report rendering'

echo '== report verifier refusals =='
expect_status 1 'report with a PASS but no hardware evidence' \
    bash -c "sed 's/^stage_1_evidence=.*/stage_1_evidence=/' '$REPORT' \
        >'$TEST_DIR/r1.txt'; bash '$REPORT_VERIFIER' '$TEST_DIR/r1.txt'"

expect_status 1 'report with an ACCEPT gate over a not-tested stage' \
    bash -c "sed 's/^gate_status=INCOMPLETE/gate_status=ACCEPT/' '$REPORT' \
        >'$TEST_DIR/r2.txt'; bash '$REPORT_VERIFIER' '$TEST_DIR/r2.txt'"

expect_status 1 'report with a missing audit line' \
    bash -c "sed '/^audit_line=/d' '$REPORT' >'$TEST_DIR/r3.txt'; \
        bash '$REPORT_VERIFIER' '$TEST_DIR/r3.txt'"

expect_status 1 'report with an observed identity mismatch' \
    bash -c "sed 's/^observed_serial=.*/observed_serial=WRONG/' '$REPORT' \
        >'$TEST_DIR/r4.txt'; bash '$REPORT_VERIFIER' '$TEST_DIR/r4.txt'"

expect_status 1 'report that does not match the matrix' \
    bash -c "sed 's/^stage_2_name=.*/stage_2_name=wrong-name/' '$REPORT' \
        >'$TEST_DIR/r5.txt'; \
        bash '$REPORT_VERIFIER' '$TEST_DIR/r5.txt' '$MATRIX_SRC'"

expect_status 1 'report with a REJECT gate and no failure' \
    bash -c "sed 's/^gate_status=INCOMPLETE/gate_status=REJECT/' '$REPORT' \
        >'$TEST_DIR/r6.txt'; bash '$REPORT_VERIFIER' '$TEST_DIR/r6.txt'"

echo '== the released hardware report stays NOT TESTED =='
grep -Fq '**Status:** NOT TESTED' "$HARDWARE_REPORT"
if grep -Eq '^\| [A-Za-z0-9 /_-]+ \| PASS \|' "$HARDWARE_REPORT"; then
    echo 'the released hardware report fabricates a PASS row' >&2
    exit 1
fi

echo 'Hardware acceptance host test: OK'
