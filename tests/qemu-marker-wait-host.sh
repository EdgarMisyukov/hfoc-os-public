#!/usr/bin/env bash
# ITEM-609 host regression: the production update proof's marker waiter.
#
# The update proof used to wait only for its success marker, so a real terminal
# refusal in the guest's serial trace (the coordinator's Toshiba ISO against the
# fixture's QM00001 disk) became a generic 240-second marker timeout.  The
# waiter now reads the production refusal line and ends the phase with that
# reason instead of a deadline.
#
# This proof drives the same script the QEMU proof uses, with fixture serial
# logs, so the decision is tested without a guest and without a copy of the
# algorithm:
#   * a positive success is still a success;
#   * an unexpected terminal refusal fails at once with the reason line;
#   * the refusal a negative scenario expects is a success for that scenario;
#   * a partial refusal line is not terminal (the guest may still be printing);
#   * a kernel panic is reported as a panic, not a timeout;
#   * a marker that never appears is a bounded marker timeout.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

WAITER=scripts/hfoc-qemu-marker-wait.py
SERIAL_VIEW=scripts/hfoc-serial-view.py
TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

fail() {
    echo "qemu marker waiter: $*" >&2
    exit 1
}

# A refusal line is terminal only when the production renderer finished it:
# every refusal record ends with ` writes=0`.  The fixture below is the real
# shape of the line the coordinator's run produced.
cat >"$TEST_DIR/refusal.log" <<'EOF'
[PRODUCTION-INSTALL] media token=PRESENT confirm=ITEM-394-UNATTENDED-INSTALL target=TESTNVME000001 management=management-interface mode=update network=dhcp forward=default entry=unattended
[PRODUCTION-INSTALL] candidate disk=1 controller=SATA identity=QM00001 model=QEMU HARDDISK blocks=1048576 size_mib=512 selected=0
[PRODUCTION-INSTALL] refused reason=TARGET_NOT_FOUND target=TESTNVME000001 writes=0
HFOC OS 1.0.0 INITIALISED
EOF

cat >"$TEST_DIR/applied.log" <<'EOF'
[PRODUCTION-INSTALL] state preserve preflight=VERIFIED
[PRODUCTION-INSTALL] applied disk=1 identity=QM00001 controller=SATA management=local-only mode=update when_uptime_s=0 layout=VERIFIED
EOF

cat >"$TEST_DIR/controller-refusal.log" <<'EOF'
[PRODUCTION-INSTALL] state preserve refused=RANGE_MISMATCH current=PRESENT 400000+2048 plan=1000+2048 writes=0
[PRODUCTION-INSTALL] refused reason=CONTROLLER status=PRODUCTION_INSTALLER_STATE_NOT_PRESERVABLE writes=0
EOF

cat >"$TEST_DIR/panic.log" <<'EOF'
[PRODUCTION-INSTALL] media token=PRESENT confirm=ITEM-394-UNATTENDED-INSTALL target=QM00001
*** HFOC KERNEL PANIC ***
EOF

cat >"$TEST_DIR/partial-refusal.log" <<'EOF'
[PRODUCTION-INSTALL] refused reason=CONT
EOF

: >"$TEST_DIR/absent.log"

# run_waiter NAME EXPECTED_STATUS MARKER TIMEOUT LOG
#
# Sets WAITER_STATUS and WAITER_OUTPUT; returns 0 and the elapsed milliseconds
# in WAITER_MS.
run_waiter() {
    local name=$1
    local expected=$2
    local marker=$3
    local timeout=$4
    local log=$5
    local started finished

    started=$(date +%s%N)
    if WAITER_OUTPUT=$(python3 "$WAITER" "$log" "$marker" "$timeout" \
        "$SERIAL_VIEW" 2>&1); then
        WAITER_STATUS=0
    else
        WAITER_STATUS=$?
    fi
    finished=$(date +%s%N)
    WAITER_MS=$(((finished - started) / 1000000))
    [[ $WAITER_STATUS == "$expected" ]] || {
        fail "$name: exit $WAITER_STATUS, expected $expected: $WAITER_OUTPUT"
    }
}

expect_output() {
    local name=$1
    local needle=$2
    grep -Fq -- "$needle" <<<"$WAITER_OUTPUT" || {
        fail "$name: output does not carry '$needle': $WAITER_OUTPUT"
    }
}

expect_no_output() {
    local name=$1
    local needle=$2
    if grep -Fq -- "$needle" <<<"$WAITER_OUTPUT"; then
        fail "$name: output carries '$needle': $WAITER_OUTPUT"
    fi
}

# 1. Positive success.
run_waiter success 0 "[PRODUCTION-INSTALL] applied" 30 "$TEST_DIR/applied.log"
expect_no_output success "HFOC-QEMU-MARKER-TIMEOUT"
printf '  ok   success: the applied marker completes the wait\n'

# 2. Unexpected terminal refusal: fail fast with the concrete production line.
run_waiter refusal 3 "[PRODUCTION-INSTALL] applied" 30 "$TEST_DIR/refusal.log"
expect_output refusal "HFOC-QEMU-TERMINAL-REFUSAL"
expect_output refusal \
    "refused reason=TARGET_NOT_FOUND target=TESTNVME000001 writes=0"
expect_no_output refusal "HFOC-QEMU-MARKER-TIMEOUT"
(( WAITER_MS < 10000 )) || fail \
    "refusal: the terminal refusal took ${WAITER_MS}ms instead of failing at once"
printf '  ok   refusal: TARGET_NOT_FOUND ends the wait with its reason\n'

# 3. The refusal a negative scenario expects is that scenario's success.
run_waiter expected 0 "[PRODUCTION-INSTALL] refused reason=CONTROLLER" 30 \
    "$TEST_DIR/controller-refusal.log"
printf '  ok   expected refusal: the CONTROLLER refusal is the awaited marker\n'

# 4. A refusal line still being printed is not terminal.
run_waiter partial 1 "[PRODUCTION-INSTALL] applied" 1 \
    "$TEST_DIR/partial-refusal.log"
expect_output partial "HFOC-QEMU-MARKER-TIMEOUT"
printf '  ok   partial: an unfinished refusal line is not a terminal refusal\n'

# 5. Panic is named as a panic, not as a deadline.
run_waiter panic 2 "[PRODUCTION-INSTALL] applied" 30 "$TEST_DIR/panic.log"
expect_output panic "kernel panic while waiting for"
expect_no_output panic "HFOC-QEMU-MARKER-TIMEOUT"
printf '  ok   panic: the panic ends the wait and is named\n'

# 6. Absence is a bounded marker timeout, with the canonical ITEM-412 record.
run_waiter absent 1 "[PRODUCTION-INSTALL] applied" 1 "$TEST_DIR/absent.log"
expect_output absent \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[PRODUCTION-INSTALL] applied" deadline=1s'
printf '  ok   absent: the wait stays bounded and declares the timeout class\n'

# 7. A negative phase freezes the emulator as soon as the expected terminal
#    record appears.  The guest continues into the installed appliance after a
#    refusal, and that later boot writes its own runtime state; freezing the
#    vCPU here keeps those writes outside the "refusal wrote nothing" proof.
sleep 30 &
victim=$!
if WAITER_OUTPUT=$(python3 "$WAITER" "$TEST_DIR/controller-refusal.log" \
    "[PRODUCTION-INSTALL] refused reason=CONTROLLER" 30 "$SERIAL_VIEW" \
    --stop-pid "$victim" 2>&1); then
    WAITER_STATUS=0
else
    WAITER_STATUS=$?
fi
[[ $WAITER_STATUS == 0 ]] || {
    fail "freeze: exit $WAITER_STATUS: $WAITER_OUTPUT"
}
# SIGSTOP is delivered synchronously to the target, but the scheduler can
# publish the stopped state a moment later; give it a bounded second.
victim_state=
for _ in {1..100}; do
    victim_state=$(ps -o stat= -p "$victim" | tr -d '[:space:]')
    [[ $victim_state == T* ]] && break
    sleep 0.01
done
[[ $victim_state == T* ]] || {
    fail "freeze: the waiter did not stop pid $victim (state '$victim_state')"
}
kill -CONT "$victim" 2>/dev/null || true
kill -KILL "$victim" 2>/dev/null || true
wait "$victim" 2>/dev/null || true
printf '  ok   freeze: the expected refusal stops the emulator before it boots on\n'

# 8. A pid of zero or below addresses a process group, not one emulator, and
#    must be refused before any signal is sent.
if WAITER_OUTPUT=$(python3 "$WAITER" "$TEST_DIR/applied.log" \
    "[PRODUCTION-INSTALL] applied" 30 "$SERIAL_VIEW" --stop-pid 0 2>&1); then
    WAITER_STATUS=0
else
    WAITER_STATUS=$?
fi
[[ $WAITER_STATUS == 2 ]] || {
    fail "invalid pid: exit $WAITER_STATUS, expected 2: $WAITER_OUTPUT"
}
printf '  ok   invalid pid: a process-group target is refused as usage\n'

echo "qemu marker waiter: OK"
