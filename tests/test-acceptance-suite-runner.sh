#!/usr/bin/env bash
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

[[ $# == 1 ]] || {
    echo 'acceptance runner must invoke one make target' >&2
    exit 64
}

target=$1
printf 'start %s\n' "$target" >>"$FAKE_STATE/events"

case "$target" in
    serial-before|serial-after|slow-a|slow-b)
        printf '%s\n' "$$" >>"$FAKE_STATE/pids"
        [[ "$target" == serial-before || "$target" == serial-after ]] || sleep 0.25
        printf 'marker %s\n' "$target"
        printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
        ;;
    fail)
        exit 23
        ;;
    timing-once)
        # ITEM-421: a marker timeout on the first attempt, a pass on the retry.
        if [[ -f $FAKE_STATE/timing-once-attempts ]]; then
            printf 'marker timing-once\n'
            printf 'end timing-once\n' >>"$FAKE_STATE/events"
        else
            : >"$FAKE_STATE/timing-once-attempts"
            printf 'HFOC-QEMU-MARKER-TIMEOUT marker="[TIMING] ok" deadline=5s\n' >&2
            exit 2
        fi
        ;;
    assertion-fail)
        # ITEM-421: an in-guest failure record is never a timing class.
        printf '[TIMING] failed: assertion\n' >&2
        exit 2
        ;;
    missing-marker)
        printf 'not the required marker\n'
        ;;
    queued)
        printf 'marker queued\n'
        printf 'end queued\n' >>"$FAKE_STATE/events"
        ;;
    *)
        echo "unknown target: $target" >&2
        exit 64
        ;;
esac
FAKE
chmod +x "$FAKE_MAKE"

run_runner() {
    HFOC_MAKE_CMD="$FAKE_MAKE" bash scripts/run-acceptance-suite.sh "$@"
}

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
ARTIFACT_DIR=$TEST_DIR/artifacts-success
HFOC_ACCEPTANCE_JOBS=2 run_runner \
    --artifact-dir "$ARTIFACT_DIR" \
    --serial serial-before 'marker serial-before' \
    --parallel slow-a 'marker slow-a' slow-b 'marker slow-b' \
    --serial serial-after 'marker serial-after'

first_parallel_end=$(grep -n '^end slow-' "$FAKE_STATE/events" | head -n 1 | cut -d: -f1)
[[ -n "$first_parallel_end" ]] || {
    echo 'parallel success case recorded no completed slow target' >&2
    exit 1
}
starts_before_parallel_end=$(head -n "$((first_parallel_end - 1))" "$FAKE_STATE/events" |
    grep -c '^start slow-' || true)
[[ "$starts_before_parallel_end" == 2 ]] || {
    echo 'jobs=2 did not start two parallel targets before completion' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

serial_before_end=$(grep -n '^end serial-before$' "$FAKE_STATE/events" | cut -d: -f1)
first_parallel_start=$(grep -n '^start slow-' "$FAKE_STATE/events" | head -n 1 | cut -d: -f1)
last_parallel_end=$(grep -n '^end slow-' "$FAKE_STATE/events" | tail -n 1 | cut -d: -f1)
serial_after_start=$(grep -n '^start serial-after$' "$FAKE_STATE/events" | cut -d: -f1)
(( serial_before_end < first_parallel_start && last_parallel_end < serial_after_start )) || {
    echo 'serial barriers did not surround the parallel phase' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

for target in serial-before slow-a slow-b serial-after; do
    [[ -f "$ARTIFACT_DIR/$target.log" ]] || {
        echo "missing acceptance log for $target" >&2
        exit 1
    }
    grep -Fxq "PASS $target" "$ARTIFACT_DIR/manifest.txt"
done

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
HFOC_ACCEPTANCE_JOBS=1 run_runner \
    --artifact-dir "$TEST_DIR/artifacts-jobs-1" \
    --parallel slow-a 'marker slow-a' slow-b 'marker slow-b'

first_end=$(grep -n '^end slow-' "$FAKE_STATE/events" | head -n 1 | cut -d: -f1)
starts_before_end=$(head -n "$((first_end - 1))" "$FAKE_STATE/events" |
    grep -c '^start slow-' || true)
[[ "$starts_before_end" == 1 ]] || {
    echo 'jobs=1 did not serialize parallel phase' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
FAIL_ARTIFACT_DIR=$TEST_DIR/artifacts-fail
if HFOC_ACCEPTANCE_JOBS=2 run_runner \
    --artifact-dir "$FAIL_ARTIFACT_DIR" \
    --parallel fail 'marker fail' slow-a 'marker slow-a' queued 'marker queued'; then
    echo 'expected acceptance runner failure' >&2
    exit 1
fi

grep -Fxq 'start fail' "$FAKE_STATE/events"
grep -Fxq 'start slow-a' "$FAKE_STATE/events"
if grep -Fxq 'start queued' "$FAKE_STATE/events"; then
    echo 'acceptance runner started queued work after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
grep -Fxq 'end slow-a' "$FAKE_STATE/events"
grep -Fxq 'FAIL fail' "$FAIL_ARTIFACT_DIR/manifest.txt"
grep -Fxq 'PASS slow-a' "$FAIL_ARTIFACT_DIR/manifest.txt"
[[ -f "$FAIL_ARTIFACT_DIR/fail.log" && -f "$FAIL_ARTIFACT_DIR/slow-a.log" ]] || {
    echo 'failure artifacts did not preserve started target logs' >&2
    exit 1
}

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
MARKER_ARTIFACT_DIR=$TEST_DIR/artifacts-marker-fail
if HFOC_ACCEPTANCE_JOBS=1 run_runner \
    --artifact-dir "$MARKER_ARTIFACT_DIR" \
    --serial missing-marker 'marker missing-marker'; then
    echo 'expected missing marker failure' >&2
    exit 1
fi
grep -Fxq 'FAIL missing-marker' "$MARKER_ARTIFACT_DIR/manifest.txt"
[[ -f "$MARKER_ARTIFACT_DIR/missing-marker.log" ]] || {
    echo 'missing-marker failure did not preserve its log' >&2
    exit 1
}

# ITEM-421: a marker timeout is retried once, visibly, and the verdict comes
# from the retry.
: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
RETRY_ARTIFACT_DIR=$TEST_DIR/artifacts-retry
HFOC_ACCEPTANCE_JOBS=1 run_runner \
    --artifact-dir "$RETRY_ARTIFACT_DIR" \
    --serial timing-once 'marker timing-once'
grep -Fxq 'PASS timing-once' "$RETRY_ARTIFACT_DIR/manifest.txt"
grep -Fxq 'RETRY-PASS timing-once reason=marker-timeout attempt=2' \
    "$RETRY_ARTIFACT_DIR/manifest.txt"
grep -Fq 'reason=marker-timeout' "$RETRY_ARTIFACT_DIR/retries.txt"
starts=$(grep -c '^start timing-once' "$FAKE_STATE/events" || true)
[[ "$starts" == 2 ]] || {
    echo "acceptance runner started timing-once $starts times, expected 2" >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

# And a failure that is not a timing class is never retried.
: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
ASSERT_ARTIFACT_DIR=$TEST_DIR/artifacts-assertion
if HFOC_ACCEPTANCE_JOBS=1 run_runner \
    --artifact-dir "$ASSERT_ARTIFACT_DIR" \
    --serial assertion-fail 'marker assertion-fail'; then
    echo 'expected the acceptance runner to fail an assertion' >&2
    exit 1
fi
grep -Fxq 'FAIL assertion-fail' "$ASSERT_ARTIFACT_DIR/manifest.txt"
starts=$(grep -c '^start assertion-fail' "$FAKE_STATE/events" || true)
[[ "$starts" == 1 ]] || {
    echo "acceptance runner retried an assertion $starts times" >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

if pgrep -f "^$FAKE_MAKE" >/dev/null; then
    echo 'fake make child leaked after acceptance runner failure' >&2
    pgrep -af "^$FAKE_MAKE" >&2 || true
    exit 1
fi

echo 'Acceptance suite runner host test: OK'
