#!/usr/bin/env bash
# ITEM-370A focused host proof for the host-wide heavy lock.
#
# The lock is the only thing that keeps a local heavy build (the autonomous
# worker's `make ...` runs) from overlapping a heavy CI phase (the
# static-host / acceptance-qemu matrix phases). This test drives the real
# tool with real processes and a private lock file, so the proof does not
# depend on /var/lock or on the CI runner being idle:
#
#   * shared holders coexist (the two CI phases stay parallel);
#   * an exclusive local build waits for a shared CI holder and records the
#     wait in its own log line;
#   * a CI phase waits for a short local build instead of failing;
#   * the bounded wait turns a stuck holder into an explicit diagnostic and
#     a distinct exit code instead of an overlap or a hang;
#   * the lock file descriptor is not inherited by the command's children,
#     so a leaked background process cannot hold the lock after the command
#     exits;
#   * the local wrapper keeps the host priority settings (nice/ionice);
#   * an unusable lock path fails closed instead of running unlocked;
#   * a nested wrapper invocation does not deadlock.
# shellcheck disable=SC2016  # '$0' is the inner `bash -c` argument, not this script

set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TOOL="$ROOT/scripts/hfoc-heavy-lock.sh"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-heavy-lock-test.XXXXXX")
background_pids=()
cleanup() {
    local pid
    for pid in "${background_pids[@]:-}"; do
        [[ -n "$pid" ]] || continue
        kill "$pid" 2>/dev/null || true
    done
    wait 2>/dev/null || true
    rm -rf "$WORK"
}
trap cleanup EXIT

checks=0
fail() {
    echo "heavy lock host test: FAIL: $*" >&2
    exit 1
}
ok() {
    checks=$((checks + 1))
}

now_ms() {
    date +%s%3N
}

wait_for_file() {
    local file=$1
    local tries=400
    while (( tries > 0 )); do
        [[ -e "$file" ]] && return 0
        sleep 0.05
        tries=$((tries - 1))
    done
    return 1
}

wait_for_pattern() {
    local file=$1
    local pattern=$2
    local tries=400
    while (( tries > 0 )); do
        grep -Fq "$pattern" "$file" 2>/dev/null && return 0
        sleep 0.05
        tries=$((tries - 1))
    done
    return 1
}

[[ -x "$TOOL" ]] || fail "$TOOL is missing or not executable"

export HFOC_HEAVY_LOCK="$WORK/heavy.lock"

# 1. A shared CI holder takes the lock immediately when it is free and
#    publishes waiting/acquired/released lines.
out=$("$TOOL" ci-shared static-host -- true 2>&1) ||
    fail "ci-shared acquire failed on a free lock"
grep -Fq 'HFOC_HEAVY_LOCK state=waiting mode=shared phase=static-host' <<<"$out" ||
    fail "ci-shared did not publish the waiting line: $out"
grep -Fq 'HFOC_HEAVY_LOCK state=acquired mode=shared phase=static-host' <<<"$out" ||
    fail "ci-shared did not publish the acquired line: $out"
grep -Fq 'HFOC_HEAVY_LOCK state=released mode=shared phase=static-host' <<<"$out" ||
    fail "ci-shared did not publish the released line: $out"
ok

# 2. Two shared holders coexist: the acceptance-qemu phase must not wait for
#    the static-host phase (the measured-good parallel pair).
"$TOOL" ci-shared static-host -- \
    bash -c 'touch "$0/holding"; sleep 4' "$WORK" >"$WORK/shared-holder.log" 2>&1 &
shared_holder=$!
background_pids+=("$shared_holder")
wait_for_file "$WORK/holding" || fail "shared holder never started its command"
started=$(now_ms)
out=$("$TOOL" ci-shared acceptance-qemu -- true 2>&1)
elapsed_ms=$(( $(now_ms) - started ))
(( elapsed_ms < 2000 )) ||
    fail "second shared holder waited ${elapsed_ms}ms for the first one"
grep -Fq 'HFOC_HEAVY_LOCK state=acquired mode=shared phase=acceptance-qemu' <<<"$out" ||
    fail "second shared holder did not acquire"
kill -0 "$shared_holder" 2>/dev/null ||
    fail "shared holder exited before the parallel acquire was proven"
wait "$shared_holder" || true
ok

# 3. An exclusive local build waits for a live shared holder and records the
#    measured wait in its own log, which is the runtime proof the plan asks
#    for.
rm -f "$WORK/holding"
"$TOOL" ci-shared static-host -- \
    bash -c 'touch "$0/holding"; sleep 3' "$WORK" >"$WORK/shared-holder.log" 2>&1 &
shared_holder=$!
background_pids+=("$shared_holder")
wait_for_file "$WORK/holding" || fail "shared holder never started its command"
started=$(now_ms)
out=$("$TOOL" local -- true 2>&1)
elapsed_ms=$(( $(now_ms) - started ))
acquired_line=$(grep -F 'HFOC_HEAVY_LOCK state=acquired mode=exclusive' <<<"$out") ||
    fail "local build did not acquire after the shared holder: $out"
wait_s=$(sed -n 's/.*wait_s=\([0-9][0-9]*\).*/\1/p' <<<"$acquired_line")
[[ -n "$wait_s" ]] || fail "acquired line has no wait_s: $acquired_line"
(( wait_s >= 1 )) || fail "local build reported wait_s=$wait_s for a 3s holder"
(( elapsed_ms >= 1500 )) ||
    fail "local build returned after ${elapsed_ms}ms without waiting"
wait "$shared_holder" || true
ok

# 4. A bounded wait turns a stuck holder into an explicit diagnostic with a
#    distinct exit code; the holder is named so the operator can act.
rm -f "$WORK/holding"
"$TOOL" ci-shared acceptance-qemu -- \
    bash -c 'touch "$0/holding"; sleep 20' "$WORK" >"$WORK/stuck-holder.log" 2>&1 &
stuck_holder=$!
background_pids+=("$stuck_holder")
wait_for_pattern "$WORK/stuck-holder.log" 'state=acquired' ||
    fail "stuck holder never acquired the lock"
set +e
"$TOOL" local --wait 1 -- true >"$WORK/timeout.out" 2>"$WORK/timeout.err"
rc=$?
set -e
[[ "$rc" -eq 75 ]] || fail "bounded wait exit code was $rc, expected 75"
grep -Fq 'HFOC_HEAVY_LOCK state=timeout mode=exclusive' "$WORK/timeout.err" ||
    fail "timeout did not publish a timeout line: $(cat "$WORK/timeout.err")"
grep -Fq 'holder:' "$WORK/timeout.err" ||
    fail "timeout diagnostic does not name the holder: $(cat "$WORK/timeout.err")"
grep -Fq 'phase=acceptance-qemu' "$WORK/timeout.err" ||
    fail "timeout diagnostic does not name the holding phase: $(cat "$WORK/timeout.err")"
kill "$stuck_holder" 2>/dev/null || true
wait "$stuck_holder" 2>/dev/null || true
ok

# 5. A CI phase waits for a short local build instead of failing or
#    overlapping it.
rm -f "$WORK/holding"
"$TOOL" local -- bash -c 'touch "$0/holding"; sleep 3' "$WORK" >"$WORK/local-holder.log" 2>&1 &
local_holder=$!
background_pids+=("$local_holder")
wait_for_file "$WORK/holding" || fail "local holder never started its command"
started=$(now_ms)
out=$("$TOOL" ci-shared static-host -- true 2>&1)
elapsed_ms=$(( $(now_ms) - started ))
(( elapsed_ms >= 1500 )) ||
    fail "ci-shared returned after ${elapsed_ms}ms while a local build held the lock"
grep -Fq 'HFOC_HEAVY_LOCK state=acquired mode=shared' <<<"$out" ||
    fail "ci-shared did not acquire after the local build"
wait "$local_holder" || true
ok

# 6. The lock descriptor must not leak into the command's children: a
#    background process that outlives the command must not keep the lock.
"$TOOL" local -- bash -c 'sleep 20 >/dev/null 2>&1 & exit 0'
out=$("$TOOL" local --wait 2 -- true) ||
    fail "lock was still held by a leaked background child after the wrapper exited"
ok

# 7. The local wrapper keeps the host priority settings and lets them be
#    tuned. `nice -n` is an increment on top of the caller's niceness (the CI
#    runner already runs at nice 10), so the expectation is relative; the
#    kernel caps the result at 19.
parent_nice=$(awk '{print $19}' /proc/self/stat)
nice_value=$(HFOC_HEAVY_NICE=7 "$TOOL" local -- awk '{print $19}' /proc/self/stat) ||
    fail "local wrapper could not run a command"
expected_nice=$((parent_nice + 7))
(( expected_nice <= 19 )) || expected_nice=19
[[ "$nice_value" == "$expected_nice" ]] ||
    fail "local wrapper did not add HFOC_HEAVY_NICE=7 to nice $parent_nice (got '$nice_value', expected '$expected_nice')"
grep -Fq 'HFOC_HEAVY_RUN state=start' <<<"$(HFOC_HEAVY_NICE=7 "$TOOL" local -- true 2>&1)" ||
    fail "local wrapper did not publish its start line"
ok

# 8. An unusable lock path fails closed: the command must not run unlocked.
set +e
HFOC_HEAVY_LOCK="$WORK/missing-dir/heavy.lock" \
    "$TOOL" local -- bash -c 'touch "$0/ran-unlocked"' "$WORK" \
    >"$WORK/config.out" 2>"$WORK/config.err"
rc=$?
set -e
[[ "$rc" -eq 78 ]] || fail "unusable lock path exit code was $rc, expected 78"
[[ ! -e "$WORK/ran-unlocked" ]] ||
    fail "command ran even though the lock path was unusable"
grep -Fq 'missing-dir/heavy.lock' "$WORK/config.err" ||
    fail "unusable lock path diagnostic does not name the path: $(cat "$WORK/config.err")"
ok

# 9. A nested wrapper invocation runs reentrantly instead of deadlocking on
#    the exclusive lock its parent already holds.
out=$("$TOOL" local -- "$TOOL" local -- true 2>&1) ||
    fail "nested local wrapper invocation deadlocked or failed"
grep -Fq 'state=reentrant' <<<"$out" ||
    fail "nested local wrapper invocation did not report reentrancy: $out"
ok

# 10. Every published line is a single-line marker with one state field, so a
#     reader can parse the lock history without splitting lines.
out=$("$TOOL" ci-shared acceptance-qemu -- true 2>&1)
while IFS= read -r line; do
    [[ "$line" == HFOC_HEAVY_LOCK\ * ]] || continue
    states=$(grep -o 'state=' <<<"$line" | wc -l)
    [[ "$states" -eq 1 ]] || fail "marker line carries $states state fields: $line"
    grep -Eq '^HFOC_HEAVY_LOCK state=[a-z]+ mode=[a-z]+ phase=[a-z-]+ lock=[^ ]+( [a-z_]+=[^ ]+)*$' <<<"$line" ||
        fail "marker line has an unexpected shape: $line"
done <<<"$out"
ok

# 11. The holder registry is cleaned up on release, so a later timeout
#     diagnostic cannot name a dead holder.
if [[ -d "$HFOC_HEAVY_LOCK.holders" ]]; then
    live=0
    for holder in "$HFOC_HEAVY_LOCK.holders"/*; do
        [[ -e "$holder" ]] || continue
        pid=$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' "$holder")
        [[ -n "$pid" && -d "/proc/$pid" ]] && live=$((live + 1))
    done
    (( live == 0 )) || fail "$live holder entries survived their release"
fi
ok

# 12. A holder file left behind by a killed holder (a cancelled CI job never
#     runs its EXIT trap) is pruned by the next acquisition, so the registry
#     cannot accumulate dead entries that a timeout diagnostic would have to
#     filter out.
mkdir -p "$HFOC_HEAVY_LOCK.holders"
dead_pid=$(bash -c 'sleep 0.2 & echo $!')
for _ in $(seq 1 100); do
    [[ -d "/proc/$dead_pid" ]] || break
    sleep 0.1
done
[[ ! -d "/proc/$dead_pid" ]] || fail "test setup: pid $dead_pid is still alive"
printf 'pid=%s mode=shared phase=acceptance-qemu user=hfoc-runner started_epoch=0\n' \
    "$dead_pid" >"$HFOC_HEAVY_LOCK.holders/$dead_pid.stale"
out=$("$TOOL" ci-shared static-host -- true 2>&1) ||
    fail "ci-shared failed while a stale holder file was present"
[[ ! -e "$HFOC_HEAVY_LOCK.holders/$dead_pid.stale" ]] ||
    fail "a stale holder file survived the next acquisition"
ok

# 13. ITEM-431: the exclusive CI wait is longer than the longest live phase,
#     while a shared holder keeps the short bound; both stay overridable, and
#     the override the caller sets is the one the marker publishes.
out=$("$TOOL" ci-exclusive postmerge -- true 2>&1) ||
    fail "ci-exclusive failed on a free lock"
grep -Fq 'state=waiting mode=exclusive phase=postmerge' <<<"$out" ||
    fail "the exclusive wait is not the post-merge phase: $out"
grep -Fq 'wait_s=4500' <<<"$out" ||
    fail "the exclusive CI wait is not the post-merge budget: $out"
out=$("$TOOL" ci-shared static-host -- true 2>&1) ||
    fail "ci-shared failed on a free lock"
grep -Fq 'wait_s=1200' <<<"$out" ||
    fail "the shared CI wait changed with the exclusive one: $out"
out=$(HFOC_HEAVY_CI_EXCLUSIVE_WAIT=7 "$TOOL" ci-exclusive postmerge -- true 2>&1) ||
    fail "the exclusive CI wait override broke the run"
grep -Fq 'wait_s=7' <<<"$out" ||
    fail "the exclusive CI wait override was not honoured: $out"
out=$(HFOC_HEAVY_CI_WAIT=9 "$TOOL" ci-exclusive postmerge -- true 2>&1) ||
    fail "the generic CI wait override broke the run"
grep -Fq 'wait_s=9' <<<"$out" ||
    fail "the generic CI wait override was not honoured: $out"
# The specific override wins over the generic one when both are set.
out=$(HFOC_HEAVY_CI_WAIT=9 HFOC_HEAVY_CI_EXCLUSIVE_WAIT=5 \
    "$TOOL" ci-exclusive postmerge -- true 2>&1) ||
    fail "both CI wait overrides together broke the run"
grep -Fq 'wait_s=5' <<<"$out" ||
    fail "the generic CI wait override beat the exclusive one: $out"
ok

echo "Heavy lock host test: OK ($checks checks)"
