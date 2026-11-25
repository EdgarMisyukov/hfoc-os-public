#!/usr/bin/env bash
# Host regression harness for the bounded host suite scheduler.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
FAKE_MAKE=$TEST_DIR/fake-make
FAKE_STATE=$TEST_DIR/state
export FAKE_STATE
mkdir -p "$FAKE_STATE"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if [[ -f "$FAKE_STATE/pids" ]]; then
        while read -r pid; do
            kill "$pid" 2>/dev/null || true
        done <"$FAKE_STATE/pids"
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cat >"$FAKE_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

state=$FAKE_STATE
[[ $# == 1 ]] || {
    echo "host runner must invoke one make target" >&2
    exit 64
}
target=$1
printf 'start %s\n' "$target" >>"$state/events"

case "$target" in
    slow-a|slow-b)
        printf '%s\n' "$$" >>"$state/pids"
        sleep 0.25
        printf 'end %s\n' "$target" >>"$state/events"
        ;;
    fail)
        exit 23
        ;;
    queued)
        printf 'end queued\n' >>"$state/events"
        ;;
    *)
        printf 'unknown target: %s\n' "$target" >&2
        exit 64
        ;;
esac
FAKE
chmod +x "$FAKE_MAKE"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
HFOC_CI_TIMINGS_DIR="$TEST_DIR/untimed" HFOC_HOST_SUITE_ARTIFACT_DIR='' HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_HOST_JOBS=2 \
    bash scripts/run-host-suite.sh --parallel slow-a slow-b

first_end=$(grep -n '^end ' "$FAKE_STATE/events" | head -n 1 | cut -d: -f1)
[[ -n "$first_end" ]] || {
    echo 'success case recorded no completed target' >&2
    exit 1
}
starts_before_end=$(head -n "$((first_end - 1))" "$FAKE_STATE/events" |
    grep -c '^start ' || true)
[[ "$starts_before_end" == 2 ]] || {
    echo 'parallel case did not start two targets before the first completion' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fxq 'end slow-a' "$FAKE_STATE/events"
grep -Fxq 'end slow-b' "$FAKE_STATE/events"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
HFOC_CI_TIMINGS_DIR="$TEST_DIR/untimed" HFOC_HOST_SUITE_ARTIFACT_DIR='' HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_HOST_JOBS=1 \
    bash scripts/run-host-suite.sh --parallel slow-a slow-b

first_end=$(grep -n '^end ' "$FAKE_STATE/events" | head -n 1 | cut -d: -f1)
starts_before_end=$(head -n "$((first_end - 1))" "$FAKE_STATE/events" |
    grep -c '^start ' || true)
[[ "$starts_before_end" == 1 ]] || {
    echo 'HFOC_HOST_JOBS=1 did not serialize target starts' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

SUITE_ARTIFACT_DIR=$TEST_DIR/suite-artifacts
HFOC_CI_TIMINGS_DIR="$TEST_DIR/untimed" \
    HFOC_HOST_SUITE_ARTIFACT_DIR="$SUITE_ARTIFACT_DIR" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_HOST_JOBS=2 \
    bash scripts/run-host-suite.sh --serial slow-a
[[ -f "$SUITE_ARTIFACT_DIR/slow-a.log" ]] || {
    echo 'host artifact capture did not preserve successful log' >&2
    exit 1
}
grep -Fxq 'slow-a PASS' "$SUITE_ARTIFACT_DIR/suite-status.txt"

rm -rf "$SUITE_ARTIFACT_DIR"
: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_CI_TIMINGS_DIR="$TEST_DIR/untimed" \
    HFOC_HOST_SUITE_ARTIFACT_DIR="$SUITE_ARTIFACT_DIR" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_HOST_JOBS=2 \
    bash scripts/run-host-suite.sh --serial fail; then
    echo 'expected serial host suite failure with artifacts' >&2
    exit 1
fi
[[ -f "$SUITE_ARTIFACT_DIR/fail.log" ]] || {
    echo 'host artifact capture did not preserve failure log' >&2
    exit 1
}
grep -Fxq 'fail FAIL' "$SUITE_ARTIFACT_DIR/suite-status.txt"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_CI_TIMINGS_DIR="$TEST_DIR/untimed" \
    HFOC_HOST_SUITE_ARTIFACT_DIR='' HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_HOST_JOBS=2 \
    bash scripts/run-host-suite.sh --parallel fail slow-a queued; then
    echo 'expected host suite failure' >&2
    exit 1
fi

grep -Fxq 'start fail' "$FAKE_STATE/events"
grep -Fxq 'start slow-a' "$FAKE_STATE/events"
if grep -Fxq 'start queued' "$FAKE_STATE/events"; then
    echo 'host suite started a queued target after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
grep -Fxq 'end slow-a' "$FAKE_STATE/events"

if pgrep -f "^$FAKE_MAKE" >/dev/null; then
    echo 'fake make child leaked after host suite failure' >&2
    pgrep -af "^$FAKE_MAKE" >&2 || true
    exit 1
fi

# ITEM-585: timing records.  A parallel run writes one line per target, the
# phase wrapper is its own record, failures keep their return code, and a
# second attempt writes into its own directory (no record from attempt 1 can
# be read as attempt 2's).
TIMINGS=$TEST_DIR/timings
timing_file_for() {
    printf '%s/%s-%s-%s/host-suite.tsv' "$TIMINGS" "$1" "$2" "$3"
}
: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
HFOC_CI_TIMINGS_DIR="$TIMINGS" GITHUB_JOB=fixture \
    GITHUB_RUN_ID=9001 GITHUB_RUN_ATTEMPT=1 GITHUB_SHA=deadbeef \
    HFOC_HOST_SUITE_ARTIFACT_DIR='' HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_HOST_JOBS=2 \
    bash scripts/run-host-suite.sh --parallel slow-a slow-b
first_timing=$(timing_file_for fixture 9001 1)
[[ -f $first_timing ]] || {
    echo 'timed host run wrote no timing record' >&2
    exit 1
}
[[ $(grep -c '^fixture	1	deadbeef	static-host	host-target	' "$first_timing") == 2 ]] || {
    echo 'parallel timed run did not record both targets' >&2
    cat "$first_timing" >&2
    exit 1
}
grep -Fq 'host-target	slow-a	PASS	0	' "$first_timing" || {
    echo 'timed run did not record slow-a PASS' >&2
    exit 1
}
grep -Fq 'host-target	slow-b	PASS	0	' "$first_timing" || {
    echo 'timed run did not record slow-b PASS' >&2
    exit 1
}
[[ $(grep -c '^fixture	1	deadbeef	static-host	phase-wrapper	host-suite	PASS	0	' "$first_timing") == 1 ]] || {
    echo 'timed run did not record exactly one PASS phase wrapper' >&2
    cat "$first_timing" >&2
    exit 1
}
awk -F'\t' 'NF != 11 { exit 1 }' "$first_timing" || {
    echo 'a timing record does not carry the eleven columns' >&2
    cat "$first_timing" >&2
    exit 1
}

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_CI_TIMINGS_DIR="$TIMINGS" GITHUB_JOB=fixture \
    GITHUB_RUN_ID=9001 GITHUB_RUN_ATTEMPT=1 GITHUB_SHA=deadbeef \
    HFOC_HOST_SUITE_ARTIFACT_DIR='' HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_HOST_JOBS=2 \
    bash scripts/run-host-suite.sh --serial fail; then
    echo 'expected a timed failing host run' >&2
    exit 1
fi
grep -Fq 'host-target	fail	FAIL	23	' "$first_timing" || {
    echo 'timed failing run lost the target return code' >&2
    cat "$first_timing" >&2
    exit 1
}
grep -Fq 'phase-wrapper	host-suite	FAIL	23	' "$first_timing" || {
    echo 'timed failing run lost the phase wrapper return code' >&2
    cat "$first_timing" >&2
    exit 1
}

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
HFOC_CI_TIMINGS_DIR="$TIMINGS" GITHUB_JOB=fixture \
    GITHUB_RUN_ID=9001 GITHUB_RUN_ATTEMPT=2 GITHUB_SHA=deadbeef \
    HFOC_HOST_SUITE_ARTIFACT_DIR='' HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_HOST_JOBS=1 \
    bash scripts/run-host-suite.sh --serial slow-a
second_timing=$(timing_file_for fixture 9001 2)
[[ -f $second_timing ]] || {
    echo 'the second attempt wrote no separate timing file' >&2
    exit 1
}
[[ $(grep -c '^fixture	2	deadbeef	static-host	host-target	slow-a	PASS	0	' "$second_timing") == 1 ]] || {
    echo 'the second attempt did not record its own run/attempt columns' >&2
    cat "$second_timing" >&2
    exit 1
}
if grep -q 'fail	FAIL' "$second_timing"; then
    echo 'the second attempt read the first attempt''s failing record' >&2
    exit 1
fi

echo 'Host suite runner host test: OK'
