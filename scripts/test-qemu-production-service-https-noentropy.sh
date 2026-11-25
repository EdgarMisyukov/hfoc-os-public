#!/usr/bin/env bash
# ITEM-585: the no-RNG/operator-seed proof, extracted from
# test-qemu-production-service-https.sh (lines 3571-3701 on c1f694c1) as its
# own scheduling unit.
#
# The scenario has no disk: it boots the production ISO with a CPU that
# advertises no hardware RNG (`-cpu qemu64`), requires the appliance to publish
# AWAITING_ENTROPY with the listener down and the console working, then types
# one fixed operator seed through the hidden console line and requires
# source=OPERATOR and a live listener, with the seed never reaching the serial
# stream.  It shares no disk state with the main HTTPS proof, so the two run as
# independent children of the QEMU suite: each exactly once, and the suite's
# success is published only after both pass.
#
# The scenario keeps its own TEST_DIR, monitor socket, logs and cleanup; every
# phase is timed with a monotonic clock into the shared ci-timings format.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_NOENTROPY_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_NOENTROPY_TEST_TIMEOUT:-420}s" \
        env HFOC_NOENTROPY_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

[[ $QEMU_MACHINE == q35 ]] || { echo "no-RNG proof must set QEMU_MACHINE=q35" >&2; exit 2; }
[[ $QEMU_SMP == 1 ]] || { echo "no-RNG proof must set QEMU_SMP=1" >&2; exit 2; }
[[ -f $IMAGE ]] || { echo "no-RNG proof requires image: $IMAGE" >&2; exit 2; }

TEST_DIR=$(mktemp -d)
FAILURE_REASON=$TEST_DIR/failure-reason.txt
FAILURE_MESSAGE=
NEGATIVE_PID=

# ITEM-585: monotonic per-phase timings in the shared ci-timings schema
# (job/attempt/tree-sha/phase/kind/name/status/rc/ms/retry/started/head/base).
timing_root=${HFOC_CI_TIMINGS_DIR:-build/test-artifacts/ci-timings}
timing_job=${GITHUB_JOB:-local}
timing_run=${GITHUB_RUN_ID:-manual}
timing_attempt=${GITHUB_RUN_ATTEMPT:-1}
timing_tree=${HFOC_CI_TREE_SHA:-${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}}
timing_head=${HFOC_CI_HEAD_SHA:-${GITHUB_HEAD_SHA:-}}
timing_base=${HFOC_CI_BASE_SHA:-}
timing_phase=production-service-https-noentropy
timing_dir=$timing_root/"$timing_job-$timing_run-$timing_attempt"
timing_file=$timing_dir/$timing_phase.tsv
started_ms=$(awk '{printf "%d", $1 * 1000}' /proc/uptime)

now_ms() { awk '{printf "%d", $1 * 1000}' /proc/uptime; }
record_timing() {
    local line
    line=$(printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s' \
        "$timing_job" "$timing_attempt" "$timing_tree" "$timing_phase" \
        "$1" "$2" "$3" "$4" "$5" "$6" "$7" "$timing_head" "$timing_base")
    mkdir -p "$timing_dir" 2>/dev/null || return 0
    (
        flock 9
        printf '%s\n' "$line" >>"$timing_file"
    ) 9>>"$timing_file.lock" 2>/dev/null || printf '%s\n' "$line" >>"$timing_file" 2>/dev/null || true
}

phase_started_ms=$started_ms
phase_begin() { phase_name=$1; phase_started_ms=$(now_ms); }
phase_end() {
    local status=$1
    record_timing proof-phase "$phase_name" \
        "$([[ $status == 0 ]] && echo PASS || echo FAIL)" "$status" \
        "$(($(now_ms) - phase_started_ms))" "" "$phase_started_ms"
}

serial_view() {
    local view=$1.view
    python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"
    printf '%s\n' "$view"
}

send_monitor_keys() {
    python3 - "$1" "$2" <<'PY'
import socket
import sys
import time

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(5.0)
    monitor.connect(sys.argv[1])
    prompt = b""
    while b"(qemu)" not in prompt:
        chunk = monitor.recv(4096)
        if not chunk:
            raise SystemExit("QEMU HMP closed before prompt")
        prompt += chunk
    for character in sys.argv[2]:
        key = "spc" if character == " " else character
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        time.sleep(0.05)
    monitor.sendall(b"sendkey ret\n")
    time.sleep(0.2)
finally:
    monitor.close()
PY
}

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/production-service-noentropy-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-no-RNG proof failed}" >"$FAILURE_REASON"
    cp -a "$FAILURE_REASON" "$artifact_dir/" 2>/dev/null || true
    if [[ -n ${NEGATIVE_SERIAL:-} && -f ${NEGATIVE_SERIAL:-} ]]; then
        cp -a "$NEGATIVE_SERIAL" "$artifact_dir/boot.log" 2>/dev/null || true
    fi
    if [[ -n ${NEGATIVE_QEMU_LOG:-} && -f ${NEGATIVE_QEMU_LOG:-} ]]; then
        cp -a "$NEGATIVE_QEMU_LOG" "$artifact_dir/qemu.log" 2>/dev/null || true
    fi
    echo "no-RNG proof artifacts: $artifact_dir" >&2
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if [[ -n $NEGATIVE_PID ]] && kill -0 "$NEGATIVE_PID" 2>/dev/null; then
        kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
        wait "$NEGATIVE_PID" 2>/dev/null || true
    fi
    (( status != 0 )) && save_failure_artifacts
    record_timing proof-wrapper "$timing_phase" \
        "$([[ $status == 0 ]] && echo PASS || echo FAIL)" "$status" \
        "$(($(now_ms) - started_ms))" "" "$started_ms"
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# ITEM-386 negative proof: a CPU that advertises no hardware RNG must leave the
# appliance with no entropy owner, no listener, and a working console.
NEGATIVE_DIR=$TEST_DIR/negative
mkdir -p "$NEGATIVE_DIR"
NEGATIVE_SERIAL=$NEGATIVE_DIR/boot.log
NEGATIVE_QEMU_LOG=$NEGATIVE_DIR/qemu.log
NEGATIVE_MONITOR_SOCKET=$NEGATIVE_DIR/monitor.sock
NEGATIVE_SEED=0123456789abcdeffedcba98765432105a3c9e17d2486b0fc6a2e840bdf13579
: >"$NEGATIVE_SERIAL"
phase_begin boot
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu qemu64 \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -netdev user,id=net0 -device "$NETDEV_DEVICE" \
    -serial "file:$NEGATIVE_SERIAL" \
    -monitor "unix:$NEGATIVE_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$NEGATIVE_QEMU_LOG" 2>&1 &
NEGATIVE_PID=$!
negative_deadline=$((SECONDS + 180))
while (( SECONDS < negative_deadline )); do
    if grep -Fq '[PRODUCTION-SERVING] v=1 state=AWAITING_ENTROPY' \
            "$(serial_view "$NEGATIVE_SERIAL")"; then
        break
    fi
    if ! kill -0 "$NEGATIVE_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq '[PRODUCTION-SERVING] v=1 state=AWAITING_ENTROPY' \
        "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="No-entropy boot did not publish AWAITING_ENTROPY"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
phase_end 0
phase_begin checks
if ! grep -Fq 'listener=down' "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="No-entropy boot raised the management listener"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
if ! grep -Fq 'entropy=NONE' "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="No-entropy boot did not name the missing source"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi

# ITEM-585: the same CPU now receives one explicit operator seed through the
# hidden console line.  The seed is fixed test material, not a secret; the
# proof requires the guest to publish source=OPERATOR and raise the listener,
# and requires the serial stream not to contain the seed at all.
phase_end 0
sleep 1
phase_begin seed-command
if ! send_monitor_keys "$NEGATIVE_MONITOR_SOCKET" "entropy seed" \
        >/dev/null 2>&1; then
    FAILURE_MESSAGE="The entropy command could not be typed through the monitor"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
negative_prompt_deadline=$((SECONDS + 30))
while (( SECONDS < negative_prompt_deadline )); do
    if grep -Fq 'type 64 hex characters' "$(serial_view "$NEGATIVE_SERIAL")"; then
        break
    fi
    sleep 0.2
done
phase_end 0
phase_begin seed
if ! send_monitor_keys "$NEGATIVE_MONITOR_SOCKET" "$NEGATIVE_SEED" \
        >/dev/null 2>&1; then
    FAILURE_MESSAGE="The operator seed could not be typed through the monitor"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
negative_seed_deadline=$((SECONDS + 60))
while (( SECONDS < negative_seed_deadline )); do
    if grep -Fq '[RNG] operator seed status=OK source=OPERATOR' \
            "$(serial_view "$NEGATIVE_SERIAL")"; then
        break
    fi
    if ! kill -0 "$NEGATIVE_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq '[RNG] operator seed status=OK source=OPERATOR' \
        "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="The operator seed did not publish source=OPERATOR"
    echo "$FAILURE_MESSAGE" >&2
    grep -F '\[RNG\]' "$NEGATIVE_SERIAL" >&2 || true
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
negative_live_deadline=$((SECONDS + 60))
while (( SECONDS < negative_live_deadline )); do
    if grep -Fq 'listener=live' "$(serial_view "$NEGATIVE_SERIAL")"; then
        break
    fi
    if ! kill -0 "$NEGATIVE_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq 'listener=live' "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="The operator seed did not raise the management listener"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
if grep -Fq "$NEGATIVE_SEED" "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="The hidden operator seed reached the serial stream"
    echo "$FAILURE_MESSAGE" >&2
    kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
    wait "$NEGATIVE_PID" 2>/dev/null || true
    exit 1
fi
phase_end 0
phase_begin termination
kill -TERM "$NEGATIVE_PID" 2>/dev/null || true
wait "$NEGATIVE_PID" 2>/dev/null || true
phase_end 0
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$NEGATIVE_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic on the no-entropy boot"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "TLS-PROOF no-RNG boot: operator seed raised source=OPERATOR and listener=live"

echo "no-RNG proof: AWAITING_ENTROPY, operator seed source=OPERATOR, listener=live"
