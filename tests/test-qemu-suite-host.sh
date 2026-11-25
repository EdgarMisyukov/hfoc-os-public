#!/usr/bin/env bash
# Host regression harness for the bounded QEMU suite scheduler.
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
[[ $# == 5 && $1 == '-o' && $2 == 'deps' && $3 == '-o' && $4 == 'iso' ]] || {
    echo "suite must skip already-prepared deps and iso" >&2
    exit 64
}
target=$5
printf 'start %s\n' "$target" >>"$state/events"

case "$target" in
    slow-a|slow-b)
        printf '%s\n' "$$" >>"$state/pids"
        sleep 0.25
        printf 'end %s\n' "$target" >>"$state/events"
        ;;
    # ITEM-510: a target long enough to be killed while it runs, so the drill
    # can prove that cleanup reaches the make behind the completion wrapper.
    long-a)
        printf '%s\n' "$$" >>"$state/pids"
        sleep 30
        printf 'end long-a\n' >>"$state/events"
        ;;
    fail)
        exit 23
        ;;
    queued)
        printf 'end queued\n' >>"$state/events"
        ;;
    # ITEM-412 fixtures. `timing-once` is the load-induced marker timeout: the
    # proof stops without seeing its marker, and the retry sees it.
    timing-once)
        if [[ -f "$state/timing-once.attempt" ]]; then
            printf '[FAKE] proof OK\n'
            printf 'end timing-once\n' >>"$state/events"
            exit 0
        fi
        : >"$state/timing-once.attempt"
        printf 'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s\n' >&2
        printf 'timed out waiting for [FAKE] proof OK\n' >&2
        exit 1
        ;;
    timing-always)
        printf 'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s\n' >&2
        printf 'timed out waiting for [FAKE] proof OK\n' >&2
        exit 1
        ;;
    assert-fail)
        printf 'assertion failed: expected 7, observed 8\n' >&2
        exit 1
        ;;
    https-main)
        printf 'end https-main\n' >>"$state/events"
        ;;
    https-noentropy)
        printf 'end https-noentropy\n' >>"$state/events"
        exit 23
        ;;
    guard-fail)
        printf 'leaked QEMU process count is not zero\n' >&2
        exit 1
        ;;
    # The shape of the observed 2026-09-14 test-local-control-qemu failure: the
    # guest reports an in-guest proof failure, so the record must not be
    # honoured even when a proof also writes one.
    in-guest-fail)
        printf '[FAKE] failed: read-only appliance status proof\n' >&2
        printf 'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s\n' >&2
        exit 1
        ;;
    panic)
        printf 'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s\n' >&2
        printf '*** HFOC KERNEL PANIC *** double fault\n' >&2
        exit 1
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
HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel slow-a slow-b

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

SUITE_ARTIFACT_DIR=$TEST_DIR/suite-artifacts
HFOC_QEMU_SUITE_ARTIFACT_DIR="$SUITE_ARTIFACT_DIR" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --serial slow-a
[[ -f "$SUITE_ARTIFACT_DIR/slow-a.log" ]] || {
    echo 'suite artifact capture did not preserve successful log' >&2
    exit 1
}
grep -Fxq 'slow-a PASS' "$SUITE_ARTIFACT_DIR/suite-status.txt"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --serial slow-a slow-b
serial_order=$(awk '$1 == "start" { print $2 }' "$FAKE_STATE/events")
[[ "$serial_order" == $'slow-a\nslow-b' ]] || {
    echo 'a serial target list was not run in declaration order' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

rm -rf "$SUITE_ARTIFACT_DIR"
: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_QEMU_SUITE_ARTIFACT_DIR="$SUITE_ARTIFACT_DIR" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --serial fail; then
    echo 'expected serial suite failure with artifacts' >&2
    exit 1
fi
[[ -f "$SUITE_ARTIFACT_DIR/fail.log" ]] || {
    echo 'suite artifact capture did not preserve failure log' >&2
    exit 1
}
grep -Fxq 'fail FAIL' "$SUITE_ARTIFACT_DIR/suite-status.txt"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --serial fail; then
    echo 'expected serial suite failure' >&2
    exit 1
fi
grep -Fxq 'start fail' "$FAKE_STATE/events"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel fail slow-a queued; then
    echo 'expected suite failure' >&2
    exit 1
fi

grep -Fxq 'start fail' "$FAKE_STATE/events"
grep -Fxq 'start slow-a' "$FAKE_STATE/events"
if grep -Fxq 'start queued' "$FAKE_STATE/events"; then
    echo 'suite started a queued target after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
grep -Fxq 'end slow-a' "$FAKE_STATE/events"

if pgrep -f "^$FAKE_MAKE" >/dev/null; then
    echo 'fake make child leaked after suite failure' >&2
    pgrep -af "^$FAKE_MAKE" >&2 || true
    exit 1
fi

# ---------------------------------------------------------------- ITEM-412 --
# The timing class is retried exactly once, visibly, and its verdict is taken
# from the retry. Every other failure keeps failing the phase immediately, with
# no retry at all.
RETRY_ARTIFACTS=$TEST_DIR/retry-artifacts
RETRY_SUMMARY=$TEST_DIR/step-summary.md

reset_state() {
    rm -rf "$FAKE_STATE"
    mkdir -p "$FAKE_STATE"
    : >"$FAKE_STATE/events"
    : >"$FAKE_STATE/pids"
    rm -rf "$RETRY_ARTIFACTS"
    : >"$RETRY_SUMMARY"
}

# 1. A timing failure passes on the retry: the phase succeeds, the log carries
#    the class, the awaited marker, the deadline and the retry record, and the
#    first attempt's log is preserved next to the retry's.
reset_state
HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
    GITHUB_STEP_SUMMARY="$RETRY_SUMMARY" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel timing-once slow-a \
    >"$TEST_DIR/timing-once.log" 2>&1 || {
    echo 'timing-class target was not retried to success' >&2
    cat "$TEST_DIR/timing-once.log" >&2
    exit 1
}
grep -Fq 'QEMU suite: FAIL-CLASS timing-once class=marker-timeout' \
    "$TEST_DIR/timing-once.log" || {
    echo 'failure class not reported for the timing class' >&2
    cat "$TEST_DIR/timing-once.log" >&2
    exit 1
}
grep -Fq 'marker="[FAKE] proof OK" deadline=7s' "$TEST_DIR/timing-once.log" || {
    echo 'the timing report does not carry the marker and the deadline' >&2
    exit 1
}
grep -Fq 'QEMU suite: RETRY timing-once reason=marker-timeout attempt=2' \
    "$TEST_DIR/timing-once.log" || {
    echo 'no retry marker in the suite log' >&2
    cat "$TEST_DIR/timing-once.log" >&2
    exit 1
}
grep -Fq 'QEMU suite: PASS timing-once attempt=2' "$TEST_DIR/timing-once.log" || {
    echo 'the retry verdict is not the phase verdict' >&2
    cat "$TEST_DIR/timing-once.log" >&2
    exit 1
}
[[ "$(grep -c '^start timing-once' "$FAKE_STATE/events")" == 2 ]] || {
    echo 'the timing class was not run exactly twice' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fq 'start slow-a' "$FAKE_STATE/events" || {
    echo 'the retry dropped the other targets of the phase' >&2
    exit 1
}
grep -Fq 'timing-once PASS' "$RETRY_ARTIFACTS/suite-status.txt" || {
    echo 'the retry verdict is missing from the suite status artifact' >&2
    exit 1
}
grep -Fq 'QEMU suite: RETRY timing-once reason=marker-timeout attempt=2' \
    "$RETRY_ARTIFACTS/retries.txt" || {
    echo 'the retry is missing from the suite artifact directory' >&2
    exit 1
}
grep -Fq 'QEMU suite: RETRY timing-once reason=marker-timeout attempt=2' \
    "$RETRY_SUMMARY" || {
    echo 'the retry is missing from the job summary' >&2
    exit 1
}
grep -Fq 'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s' \
    "$RETRY_ARTIFACTS/timing-once.attempt1.log" || {
    echo 'the failed first attempt log was not preserved' >&2
    exit 1
}

# 2. The same timing failure on the retry fails the phase after that one retry.
reset_state
if HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel timing-always \
    >"$TEST_DIR/timing-always.log" 2>&1; then
    echo 'a timing failure that repeated twice did not fail the phase' >&2
    exit 1
fi
grep -Fq 'QEMU suite: RETRY timing-always reason=marker-timeout attempt=2' \
    "$TEST_DIR/timing-always.log" || {
    echo 'the repeated timing failure was not retried once' >&2
    cat "$TEST_DIR/timing-always.log" >&2
    exit 1
}
grep -Fq 'QEMU suite: FAIL timing-always' "$TEST_DIR/timing-always.log" || {
    echo 'the repeated timing failure is not reported as a phase failure' >&2
    exit 1
}
grep -Fq 'QEMU suite: NO-RETRY timing-always class=marker-timeout reason=retry-exhausted' \
    "$TEST_DIR/timing-always.log" || {
    echo 'the exhausted retry is not reported' >&2
    cat "$TEST_DIR/timing-always.log" >&2
    exit 1
}
[[ "$(grep -c '^start timing-always' "$FAKE_STATE/events")" == 2 ]] || {
    echo 'the timing class was retried more than once' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
[[ ! -s "$RETRY_ARTIFACTS/retries.txt" ]] || {
    [[ "$(wc -l <"$RETRY_ARTIFACTS/retries.txt")" == 1 ]] || {
        echo 'more than one retry was recorded for one target' >&2
        exit 1
    }
}

# 3. An assertion failure fails immediately: one run, no retry marker.
reset_state
if HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel assert-fail \
    >"$TEST_DIR/assert-fail.log" 2>&1; then
    echo 'an assertion failure did not fail the phase' >&2
    exit 1
fi
grep -Fq 'QEMU suite: FAIL-CLASS assert-fail class=assertion' \
    "$TEST_DIR/assert-fail.log" || {
    echo 'the assertion failure was not classified as an assertion' >&2
    cat "$TEST_DIR/assert-fail.log" >&2
    exit 1
}
if grep -Fq 'QEMU suite: RETRY' "$TEST_DIR/assert-fail.log"; then
    echo 'an assertion failure was retried' >&2
    cat "$TEST_DIR/assert-fail.log" >&2
    exit 1
fi
[[ "$(grep -c '^start assert-fail' "$FAKE_STATE/events")" == 1 ]] || {
    echo 'an assertion failure was run more than once' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

# 4. A fail-closed guard fails immediately: one run, no retry marker.
reset_state
if HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel guard-fail \
    >"$TEST_DIR/guard-fail.log" 2>&1; then
    echo 'a fail-closed guard did not fail the phase' >&2
    exit 1
fi
if grep -Fq 'QEMU suite: RETRY' "$TEST_DIR/guard-fail.log"; then
    echo 'a fail-closed guard was retried' >&2
    exit 1
fi
[[ "$(grep -c '^start guard-fail' "$FAKE_STATE/events")" == 1 ]] || {
    echo 'a fail-closed guard was run more than once' >&2
    exit 1
}

# 5. A record next to a real failure is not a timing class: an in-guest
#    `[TAG] failed:` record and a panic both suppress the retry.
for suppressed in in-guest-fail panic; do
    reset_state
    if HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
        HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
        bash scripts/run-qemu-suite.sh --parallel "$suppressed" \
        >"$TEST_DIR/$suppressed.log" 2>&1; then
        echo "$suppressed did not fail the phase" >&2
        exit 1
    fi
    if grep -Fq 'QEMU suite: RETRY' "$TEST_DIR/$suppressed.log"; then
        echo "a fail-closed record was retried: $suppressed" >&2
        cat "$TEST_DIR/$suppressed.log" >&2
        exit 1
    fi
    grep -Fq 'class=assertion' "$TEST_DIR/$suppressed.log" || {
        echo "the fail-closed signature was not classified as an assertion: $suppressed" >&2
        cat "$TEST_DIR/$suppressed.log" >&2
        exit 1
    }
    [[ "$(grep -c "^start $suppressed" "$FAKE_STATE/events")" == 1 ]] || {
        echo "a fail-closed record was run more than once: $suppressed" >&2
        exit 1
    }
done

# 6. The retry budget is bounded: with HFOC_QEMU_RETRY_LIMIT=0 a timing failure
#    is not retried at all, and the reason is recorded.
reset_state
if HFOC_QEMU_RETRY_LIMIT=0 HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
    HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel timing-always \
    >"$TEST_DIR/retry-limit.log" 2>&1; then
    echo 'a timing failure with no retry budget did not fail the phase' >&2
    exit 1
fi
grep -Fq 'QEMU suite: NO-RETRY timing-always class=marker-timeout reason=retry-limit:0' \
    "$TEST_DIR/retry-limit.log" || {
    echo 'the suppressed retry does not name its reason' >&2
    cat "$TEST_DIR/retry-limit.log" >&2
    exit 1
}
[[ "$(grep -c '^start timing-always' "$FAKE_STATE/events")" == 1 ]] || {
    echo 'a suppressed retry still ran the target twice' >&2
    exit 1
}

# 7. ITEM-510: bash can reap a child and still answer `wait -n -p` without
#    naming it (run 37116056572: return 127, variable left unset, and `set -u`
#    killed the phase on a healthy tree).  The phase must take the verdict from
#    the record the child wrote before exiting, and a completion with no record
#    at all must stop the phase loudly instead of dying on an unset name.
lost_completion_run() {
    local target=$1
    local mode=$2
    local log=$3
    local status

    if (
        # The injected `wait` is imported by the suite through BASH_FUNC_wait.
        # Its first call reaps one completion and returns 127 without naming
        # it, exactly the measured shape; every later call is the builtin.
        # shellcheck disable=SC2317  # called by the suite through `wait`
        wait() {
            if [[ ${hfoc_test_lost_completion:-} == 1 ]]; then
                builtin wait "$@"
                return
            fi
            hfoc_test_lost_completion=1
            if [[ ${HFOC_QEMU_TEST_LOST_COMPLETION:-} == no-record ]]; then
                return 127
            fi
            builtin wait -n || true
            return 127
        }
        export -f wait
        HFOC_QEMU_TEST_LOST_COMPLETION=$mode \
            HFOC_QEMU_SUITE_ARTIFACT_DIR="$RETRY_ARTIFACTS" \
            HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
            bash scripts/run-qemu-suite.sh --parallel "$target" >"$log" 2>&1
    ); then
        return 0
    else
        status=$?
    fi
    return "$status"
}

# 7a. A lost completion must not swallow the verdict: the assertion still fails
#     the phase with its class, and the phase does not crash on the lost name.
reset_state
if lost_completion_run assert-fail lose "$TEST_DIR/lost-completion-assert.log"; then
    echo 'a lost completion did not fail the assertion phase' >&2
    exit 1
fi
grep -Fq 'QEMU suite: FAIL-CLASS assert-fail class=assertion' \
    "$TEST_DIR/lost-completion-assert.log" || {
    echo 'a lost completion did not classify the assertion' >&2
    cat "$TEST_DIR/lost-completion-assert.log" >&2
    exit 1
}
if grep -Fq 'unbound variable' "$TEST_DIR/lost-completion-assert.log"; then
    echo 'a lost completion crashed the phase instead of being recovered' >&2
    cat "$TEST_DIR/lost-completion-assert.log" >&2
    exit 1
fi
[[ "$(grep -c '^start assert-fail' "$FAKE_STATE/events")" == 1 ]] || {
    echo 'a lost completion re-ran the target' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}

# 7b. The recovered verdict is the child's own status: a healthy target passes.
reset_state
if ! lost_completion_run slow-a lose "$TEST_DIR/lost-completion-pass.log"; then
    echo 'a lost completion failed a healthy target' >&2
    cat "$TEST_DIR/lost-completion-pass.log" >&2
    exit 1
fi
grep -Fq 'QEMU suite: PASS slow-a' "$TEST_DIR/lost-completion-pass.log" || {
    echo 'a lost completion did not report the recovered pass' >&2
    cat "$TEST_DIR/lost-completion-pass.log" >&2
    exit 1
}

# 7c. A lost completion with no record is a harness fault, not a crash: the
#     phase stops with the usage status and names the condition.
reset_state
lost_completion_status=0
if lost_completion_run slow-a no-record "$TEST_DIR/lost-completion-none.log"; then
    echo 'a lost completion with no record did not stop the phase' >&2
    exit 1
else
    lost_completion_status=$?
fi
[[ $lost_completion_status == 2 ]] || {
    echo "a lost completion with no record exited with $lost_completion_status, not 2" >&2
    cat "$TEST_DIR/lost-completion-none.log" >&2
    exit 1
}
grep -Fq 'a child completion was lost and no record was found' \
    "$TEST_DIR/lost-completion-none.log" || {
    echo 'a lost completion with no record does not name the condition' >&2
    cat "$TEST_DIR/lost-completion-none.log" >&2
    exit 1
}

# 7d. The wrapper must not orphan the make it started: a phase killed while a
#     target runs has to take that target down with it.
reset_state
HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_QEMU_JOBS=2 \
    bash scripts/run-qemu-suite.sh --parallel long-a \
    >"$TEST_DIR/long-a.log" 2>&1 &
long_suite_pid=$!
for _ in $(seq 1 100); do
    [[ -s "$FAKE_STATE/pids" ]] && break
    sleep 0.05
done
long_target_pid=$(head -n 1 "$FAKE_STATE/pids")
[[ -n $long_target_pid ]] || {
    echo 'the long target never started' >&2
    cat "$TEST_DIR/long-a.log" >&2
    exit 1
}
kill -TERM "$long_suite_pid"
wait "$long_suite_pid" 2>/dev/null || true
for _ in $(seq 1 100); do
    kill -0 "$long_target_pid" 2>/dev/null || break
    sleep 0.05
done
if kill -0 "$long_target_pid" 2>/dev/null; then
    echo 'killing the suite left its make child running' >&2
    kill -TERM "$long_target_pid" 2>/dev/null || true
    exit 1
fi

if pgrep -f "^$FAKE_MAKE" >/dev/null; then
    echo 'fake make child leaked after the retry cases' >&2
    pgrep -af "^$FAKE_MAKE" >&2 || true
    exit 1
fi

# ITEM-585: the HTTPS proof is two canonical children scheduled by this suite's
# external queue.  Both must be in the canonical parallel list (a missing child
# is a coverage loss), and a failing child fails the suite's overall verdict.
list_text=$(awk '/^QEMU_PARALLEL_PHASE_1 :=/{found=1; print; next} \
    found && /^[[:space:]]/ {print; next} \
    found {exit}' Makefile)
for child in test-production-service-https-main-qemu \
    test-production-service-https-noentropy-qemu; do
    grep -Fq "$child" <<<"$list_text" || {
        echo "the canonical QEMU parallel list is missing $child" >&2
        exit 1
    }
done
# The public target is the standalone aggregate: it is not in the CI queue
# (both children are), and it prints the canonical marker only through the
# summary script after both children succeeded.
grep -Eq '^test-production-service-https-qemu: iso' Makefile || {
    echo 'the public HTTPS target is not the ordered aggregate' >&2
    exit 1
}
grep -Fq 'scripts/test-production-service-https-summary.sh' Makefile || {
    echo 'the aggregate does not print its marker via the summary script' >&2
    exit 1
}

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_QEMU_JOBS=2 HFOC_MAKE_CMD="$FAKE_MAKE" \
    bash scripts/run-qemu-suite.sh \
    --parallel https-main https-noentropy >/dev/null 2>&1; then
    echo 'a failing HTTPS child did not fail the suite' >&2
    exit 1
fi

echo 'QEMU suite host test: OK'
