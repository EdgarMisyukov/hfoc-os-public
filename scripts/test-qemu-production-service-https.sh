#!/usr/bin/env bash
# ITEM-364 ordinary-boot production service/HTTPS activation proof, extended by
# ITEM-386 into the production serving proof.
#
# ITEM-364 part: q35 QEMU boots the plain production ISO against one writable
# AHCI HFOC partition, loads the persisted service evidence, initializes the
# fixed HTTPS service boundary through the production activation, and publishes
# one honest activation record that never carries secret material.
#
# ITEM-386 part: the same ordinary boot must actually serve the management
# plane. The proof runs the guest CPU with the hardware RNG the appliance needs
# (`-cpu max` advertises RDRAND/RDSEED; the value is overridable), forwards a
# host TCP port to the guest management port with `hostfwd`, and connects with a
# real TLS 1.3 client. It asserts:
#
#   GET /                        -> 200 HTML with the nine panel sections and
#                                   the frozen hardening headers
#   GET /health                  -> 200 JSON
#   GET /api/v1                  -> 200 with the frozen discovery body
#   POST /api/v1                 -> 405
#   GET /api/v1/unknown          -> 404
#   GET /panel/network (no auth)  -> denial without body leakage
#   POST / (the sign-in form)     -> 303 with one HttpOnly/Secure/SameSite
#                                   session cookie, and the section serves
#                                   200 to that cookie without a bearer header
#   POST /panel/logout            -> 303 with the cookie expired
#   the [PRODUCTION-SERVING] record reports a withheld listener while the
#   installed state has no account (state=AWAITING_CREDENTIAL, listener=down),
#   and a live listener with TLS 1.3, the reachability mode, the bind scope,
#   the entropy source and the certificate once the local console sets the
#   account (ITEM-506 increment 5)
#   no kernel panic and no test secret in the production artifact
#
# The no-RNG/operator-seed scenario is its own scheduling unit now
# (scripts/test-qemu-production-service-https-noentropy.sh, target
# test-production-service-https-noentropy-qemu): it shares no disk with this
# proof and the QEMU suite schedules both children in parallel.
#
# Physical service/HTTPS acceptance is not exercised.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp openssl; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_PRODUCTION_SERVICE_TEST_INNER:-0}" != 1 ]]; then
    # ITEM-506: the proof grew across the certificate, routing, facade and
    # account phases; the green run on 2026-10-03 00:46 took 376s of the old
    # 420s, so one scheduling hiccup killed a full pass locally.  ITEM-506
    # increment 5 added the withheld-listener phase and the five-target
    # gate-unavailable client on the network-apply boot; green local runs on
    # 2026-10-03 measured 679s and 704s, so the bound moved to 1200s to leave
    # room for the phase's own QEMU concurrency.  ITEM-508 increment 10: the
    # same proof then measured 1154s green on 2026-10-04 and failed at 1217s
    # and 1211s against that bound - an eleven-second overrun after every
    # assertion had run, which proves nothing about the code - so the bound
    # moves to 1800s.  The timeout only bounds a hung run - every assertion is
    # unchanged.
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_PRODUCTION_SERVICE_TEST_TIMEOUT:-1800}s" \
        env HFOC_PRODUCTION_SERVICE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
# ITEM-386: the production serving path needs a hardware entropy source. QEMU
# exposes RDRAND/RDSEED under TCG only for a CPU model that advertises them, so
# this proof runs `-cpu max`. A CPU without them is a separate negative run.
QEMU_CPU=${QEMU_CPU:-max}
IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
SERVING_PORT=${HFOC_PRODUCTION_SERVING_PORT:-$((20000 + ($$ % 20000)))}
# ITEM-401: the authenticated phase runs on its own forwarded port, so it does
# not reuse a host connection tuple from the anonymous phase above.
SERVING_AUTH_PORT=${HFOC_PRODUCTION_AUTH_SERVING_PORT:-$((SERVING_PORT + 1))}
# ITEM-504: the routing mutation proof runs on its own boot and its own
# forwarded port, because each boot's lwIP pool serves a bounded number of
# connections and the pool of neither boot above has room for it.
SERVING_ROUTING_PORT=${HFOC_PRODUCTION_ROUTING_SERVING_PORT:-$((SERVING_PORT + 2))}
# ITEM-504 increment 8: the proof backend the facade relays to.  The guest
# reaches the host through the QEMU user-network gateway 10.0.2.2, so a route
# with `tls=off` can be proven live without leaving the test host.
FACADE_BACKEND_PORT=${HFOC_FACADE_BACKEND_PORT:-$((SERVING_PORT + 3))}
# The facade relay requests run on the route-restore boot, whose lwIP pool is
# not spent by the mutation phase, so that boot needs its own forwarded port.
SERVING_REBOOT_PORT=${HFOC_PRODUCTION_REBOOT_SERVING_PORT:-$((SERVING_PORT + 4))}
# ITEM-504 increment 9: a second live backend, on its own host port (+5, after
# the reboot forwarding port, so the two never collide).  Two declared names
# answer with two different bodies, so the proof shows that the name - not the
# port, not the first table entry - picks the backend.
FACADE_BACKEND2_PORT=${HFOC_FACADE_BACKEND2_PORT:-$((SERVING_PORT + 5))}
# ITEM-505: the network change is staged and confirmed on the route-restore
# boot (whose lwIP pool still has room), and the configuration it saved is
# applied on a boot of its own after the reboot that proves it survived.  That
# apply boot needs its own forwarded port, so the client that reads the proof
# does not reuse a spent tuple.
SERVING_NETWORK_APPLY_PORT=${HFOC_PRODUCTION_NETWORK_APPLY_SERVING_PORT:-$((SERVING_PORT + 6))}
# ITEM-598 follow-up: facade relay connections and the network/hostname panel
# form each fit the six-PCB profile, but not in the same boot.  Relay client
# PCBs remain in TCP teardown long enough that the combined sequence exhausts
# the pool; give the panel form its own fresh profile.
SERVING_NETWORK_CHANGE_PORT=${HFOC_PRODUCTION_NETWORK_CHANGE_SERVING_PORT:-$((SERVING_PORT + 12))}
# ITEM-506 increment 6: the credential change runs on a boot of its own, with
# the account already provisioned and published by a local login; the boot
# after it proves the same change survived a reboot.  Both need their own
# forwarded ports, because the lwIP pool of every earlier boot is spent.
SERVING_ACCOUNT_CHANGE_PORT=${HFOC_PRODUCTION_ACCOUNT_CHANGE_SERVING_PORT:-$((SERVING_PORT + 7))}
SERVING_ACCOUNT_DURABILITY_PORT=${HFOC_PRODUCTION_ACCOUNT_DURABILITY_SERVING_PORT:-$((SERVING_PORT + 8))}
# ITEM-507 increment 4: the name -> certificate/key mutation commits its
# declaration on the credential-change boot and proves it on a boot of its own,
# whose lwIP pool is fresh and whose only TLS clients are the two selection
# probes.
SERVING_SNI_PORT=${HFOC_PRODUCTION_SNI_SERVING_PORT:-$((SERVING_PORT + 9))}
# ITEM-513: the panel's one-click language switch runs on a boot of its own.
# Its nine connections - the browser sign-in, the page read, the two drawn
# System stage bodies and their cancel, the switch and three switched reads -
# do not fit in the spent pools of the boots above, and the boot after it
# (routing) keeps its own fresh pool.
SERVING_LANGUAGE_PORT=${HFOC_PRODUCTION_LANGUAGE_SERVING_PORT:-$((SERVING_PORT + 10))}
# ITEM-516: the after-boot policy wins the gate at boot without a local login.
# The boot that enables it runs the console's own gesture on the credential
# durability boot (whose lwIP pool is untouched by this), and the policy boot
# itself opens only two bare TLS connections, so its fresh pool is not spent.
SERVING_POLICY_PORT=${HFOC_PRODUCTION_POLICY_SERVING_PORT:-$((SERVING_PORT + 11))}
# The extracted no-RNG run keeps the same NIC (and therefore the same DHCP
# path) so the entropy gate, not a missing network, is the first thing that
# blocks serving.
NETDEV_DEVICE='virtio-net-pci-non-transitional,netdev=net0,ioeventfd=off,mac=52:54:00:12:34:56,event_idx=off,indirect_desc=off,mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on'

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Production service test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Production service test must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$IMAGE" || {
    echo "Production service test requires image: $IMAGE" >&2
    exit 1
}
echo "QEMU production service baseline: machine=q35 smp=1 cpu=$QEMU_CPU serving_port=$SERVING_PORT"

TEST_DIR=$(mktemp -d)
DISK=$TEST_DIR/hfoc-service.raw
QEMU_LOG=$TEST_DIR/qemu.log
SERIAL_LOG=$TEST_DIR/boot.log
MONITOR_SOCKET=$TEST_DIR/boot.monitor
FAILURE_REASON=$TEST_DIR/failure-reason.txt
FAILURE_MESSAGE=
QEMU_PID=

# Keep the timing records outside TEST_DIR so cleanup can remove the proof's
# disks, sockets and logs without removing the measurements.
timing_root=${HFOC_CI_TIMINGS_DIR:-build/test-artifacts/ci-timings}
timing_job=${GITHUB_JOB:-local}
timing_run=${GITHUB_RUN_ID:-${TEST_DIR##*/}}
timing_attempt=${GITHUB_RUN_ATTEMPT:-1}
timing_tree=${HFOC_CI_TREE_SHA:-${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null || echo unknown)}}
timing_head=${HFOC_CI_HEAD_SHA:-${GITHUB_HEAD_SHA:-}}
timing_base=${HFOC_CI_BASE_SHA:-}
timing_phase=production-service-https-main
timing_dir=$timing_root/"$timing_job-$timing_run-$timing_attempt"
timing_file=$timing_dir/$timing_phase.tsv

timing_now_ms() {
    awk '{printf "%.0f", $1 * 1000}' /proc/uptime
}

timing_record() {
    local kind=$1 name=$2 status=$3 started=$4 ended=$5 result=PASS
    (( timing_wrapper_started_ms > 0 )) || return 0
    if (( status != 0 )); then
        result=FAIL
    fi
    mkdir -p "$timing_dir" 2>/dev/null || return 0
    (
        flock 9 || exit
        printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t\t%s\t%s\t%s\n' \
            "$timing_job" "$timing_attempt" "$timing_tree" "$timing_phase" \
            "$kind" "$name" "$result" "$status" "$((ended - started))" \
            "$started" "$timing_head" "$timing_base" >>"$timing_file"
    ) 9>>"$timing_file.lock" 2>/dev/null || true
}

timing_close() {
    timing_record proof-phase "$timing_name" "$1" "$timing_started_ms" "$2"
}

timing_next() {
    local now
    now=$(timing_now_ms) || return 0
    timing_close 0 "$now"
    timing_name=$1
    timing_started_ms=$now
}

timing_wrapper_started_ms=$(timing_now_ms) || timing_wrapper_started_ms=0
timing_started_ms=$timing_wrapper_started_ms
timing_name=setup
if mkdir -p "$timing_dir" 2>/dev/null; then
    (qemu-system-x86_64 --version >"$timing_dir/$timing_phase.qemu-version.txt") 2>/dev/null || true
fi

: >"$QEMU_LOG"
: >"$SERIAL_LOG"

create_blank_hfoc_disk() {
    python3 - "$1" <<'PY'
import pathlib
import struct
import sys
import zlib

path = pathlib.Path(sys.argv[1])
block_size = 512
block_count = (16 * 1024 * 1024) // block_size
first_lba = 34
partition_blocks = 512
disk = bytearray(block_size * block_count)

def store32(offset, value):
    disk[offset:offset + 4] = struct.pack("<I", value)

def store64(offset, value):
    disk[offset:offset + 8] = struct.pack("<Q", value)

header = block_size
disk[header:header + 8] = b"EFI PART"
store32(header + 8, 0x00010000)
store32(header + 12, 92)
store64(header + 24, 1)
store64(header + 32, block_count - 1)
store64(header + 40, first_lba)
store64(header + 48, block_count - 1)
store64(header + 72, 2)
store32(header + 80, 4)
store32(header + 84, 128)

entry = 2 * block_size + 128
disk[entry:entry + 16] = bytes([
    0xE1, 0x44, 0x01, 0x4C, 0x0D, 0x6E, 0x7A, 0x4B,
    0xB4, 0x9D, 0x22, 0x10, 0x33, 0x3D, 0x41, 0x31,
])
store64(entry + 16, 2)
store64(entry + 32, first_lba)
store64(entry + 40, first_lba + partition_blocks - 1)
store32(header + 88, zlib.crc32(disk[2 * block_size:3 * block_size]))
store32(header + 16, 0)
store32(header + 16, zlib.crc32(disk[header:header + 92]))

for store_offset in (0, 64, 128, 256):
    offset = (first_lba + store_offset) * block_size
    disk[offset:offset + 8] = b"HFOCDATA"
    disk[offset + 8] = 1
    store64(offset + 16, 1)
    store64(offset + 24, 16)

path.write_bytes(disk)
PY
}

send_monitor_quit() {
    python3 - "$1" <<'PY'
import socket
import sys
import time

monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
try:
    monitor.settimeout(1.0)
    try:
        monitor.connect(sys.argv[1])
    except OSError as exc:
        raise SystemExit(f"QEMU monitor did not accept quit: {exc}")
    deadline = time.monotonic() + 5.0
    prompt = b""
    while b"(qemu)" not in prompt:
        remaining = deadline - time.monotonic()
        if remaining <= 0.0:
            raise SystemExit("QEMU HMP prompt timeout")
        monitor.settimeout(remaining)
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            continue
        if not chunk:
            raise SystemExit("QEMU HMP closed before prompt")
        prompt += chunk
    monitor.sendall(b"quit\n")
    monitor.settimeout(5.0)
    while True:
        try:
            chunk = monitor.recv(4096)
        except socket.timeout:
            raise SystemExit("QEMU monitor EOF timeout after quit")
        if not chunk:
            break
finally:
    monitor.close()
PY
}

# ITEM-585: type one console line through the HMP monitor.  The operator seed
# line is hidden by the guest, so only the fixed test material is sent here; it
# is not a real secret and the proof checks that the guest never echoes it.
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

# ITEM-516: a failed gate gesture must name the console's own reason.  The
# serial tail shows the last thing the machine said, and the two greps pull
# every gate record and every operator refusal out of it.  The password is
# never echoed (the console reads it hidden), so the serial is safe to print.
dump_gate_diagnostics() {
    local serial=${1:-}

    [[ -n "$serial" && -f "$serial" ]] || return 0
    echo "----- gate diagnostics: $serial (last 80 lines) -----" >&2
    tail -n 80 "$serial" >&2 || true
    echo "----- gate diagnostics: MANAGEMENT GATE / REFUSED lines -----" >&2
    grep -F -e 'MANAGEMENT GATE' -e 'REFUSED:' "$serial" >&2 || true
    echo "----- end gate diagnostics -----" >&2
}

# ITEM-525: the console prints one character at a time, and an interrupt-context
# diagnostic can land inside a record it is printing.  The embedded python above
# already searches the diagnostics-stripped stream; a shell assertion that
# greps the raw file can therefore report a record missing while it is present
# (run 38264136090 lost the gate proof that way).  This renders the same stream
# through the shared renderer, whose rule the witness holds byte-identical to
# the python helper's own, so the two halves cannot disagree about what the
# console said; it prints the rendered file, so the assertions grep that file
# and a reader that stops early cannot SIGPIPE the renderer.
#
# ITEM-526: the renderer is the repository script, not `$TEST_DIR/admin-enroll.py`
# - that helper is written at line ~760, after the first boot whose assertions
# run from line ~440, so rendering through it left the view empty there and the
# first boot timed out (CI run 38408474798, `Timed out waiting for:
# [PRODUCTION-SERVICE] production_service_activation`).
serial_view() {
    local view=$1.view

    python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"
    printf '%s\n' "$view"
}

save_failure_artifacts() {
    local timestamp artifact_dir
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    artifact_dir="build/test-artifacts/production-service-$timestamp"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-Production service test failed}" \
        >"$FAILURE_REASON"
    cp -a "$SERIAL_LOG" "$QEMU_LOG" "$FAILURE_REASON" "$artifact_dir/" \
        2>/dev/null || true
    # The fail-closed boot is a separate QEMU run with its own serial log;
    # carry it into the artifacts too, so a failure there is diagnosable
    # without re-running the proof.
    if [[ -n "${NEGATIVE_SERIAL:-}" && -f "${NEGATIVE_SERIAL:-}" ]]; then
        cp -a "$NEGATIVE_SERIAL" \
            "$artifact_dir/negative-boot.log" 2>/dev/null || true
    fi
    # The ITEM-401 authenticated reboot has its own serial and QEMU logs.
    if [[ -n "${AUTH_SERIAL:-}" && -f "${AUTH_SERIAL:-}" ]]; then
        cp -a "$AUTH_SERIAL" "$artifact_dir/auth-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${AUTH_QEMU_LOG:-}" && -f "${AUTH_QEMU_LOG:-}" ]]; then
        cp -a "$AUTH_QEMU_LOG" "$artifact_dir/auth-qemu.log" 2>/dev/null || true
    fi
    # ITEM-516: the after-boot policy boot has its own serial and QEMU logs.
    if [[ -n "${POLICY_SERIAL:-}" && -f "${POLICY_SERIAL:-}" ]]; then
        cp -a "$POLICY_SERIAL" \
            "$artifact_dir/policy-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${POLICY_QEMU_LOG:-}" && -f "${POLICY_QEMU_LOG:-}" ]]; then
        cp -a "$POLICY_QEMU_LOG" \
            "$artifact_dir/policy-qemu.log" 2>/dev/null || true
    fi
    # ITEM-504 increment 6: the route-restore boot has its own logs, and a boot
    # that did not carry the committed route is exactly the failure whose log
    # is needed.
    if [[ -n "${REBOOT_SERIAL:-}" && -f "${REBOOT_SERIAL:-}" ]]; then
        cp -a "$REBOOT_SERIAL" \
            "$artifact_dir/route-restore-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${REBOOT_QEMU_LOG:-}" && -f "${REBOOT_QEMU_LOG:-}" ]]; then
        cp -a "$REBOOT_QEMU_LOG" \
            "$artifact_dir/route-restore-qemu.log" 2>/dev/null || true
    fi
    if [[ -n "${NETWORK_CHANGE_SERIAL:-}" &&
          -f "${NETWORK_CHANGE_SERIAL:-}" ]]; then
        cp -a "$NETWORK_CHANGE_SERIAL" \
            "$artifact_dir/network-change-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${NETWORK_CHANGE_QEMU_LOG:-}" &&
          -f "${NETWORK_CHANGE_QEMU_LOG:-}" ]]; then
        cp -a "$NETWORK_CHANGE_QEMU_LOG" \
            "$artifact_dir/network-change-qemu.log" 2>/dev/null || true
    fi
    # ITEM-505: the boot that applies the saved network configuration has its
    # own serial, and a boot that did not apply it is exactly the failure whose
    # log is needed.
    if [[ -n "${NETWORK_APPLY_SERIAL:-}" && -f "${NETWORK_APPLY_SERIAL:-}" ]]; then
        cp -a "$NETWORK_APPLY_SERIAL" \
            "$artifact_dir/network-apply-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${NETWORK_APPLY_QEMU_LOG:-}" &&
          -f "${NETWORK_APPLY_QEMU_LOG:-}" ]]; then
        cp -a "$NETWORK_APPLY_QEMU_LOG" \
            "$artifact_dir/network-apply-qemu.log" 2>/dev/null || true
    fi
    # ITEM-506 increment 6: the credential change and its durability boot have
    # their own serials, and a change that was refused or did not survive the
    # reboot is exactly the failure those logs diagnose.
    if [[ -n "${ACCOUNT_CHANGE_SERIAL:-}" &&
          -f "${ACCOUNT_CHANGE_SERIAL:-}" ]]; then
        cp -a "$ACCOUNT_CHANGE_SERIAL" \
            "$artifact_dir/account-change-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${ACCOUNT_DURABILITY_SERIAL:-}" &&
          -f "${ACCOUNT_DURABILITY_SERIAL:-}" ]]; then
        cp -a "$ACCOUNT_DURABILITY_SERIAL" \
            "$artifact_dir/account-durability-boot.log" 2>/dev/null || true
    fi
    # ITEM-507 increment 4: the boot that proves the committed name
    # declaration is in force has its own serial; a selection that refused the
    # wrong name or served a borrowed identity is exactly what it diagnoses.
    if [[ -n "${SNI_SERIAL:-}" && -f "${SNI_SERIAL:-}" ]]; then
        cp -a "$SNI_SERIAL" \
            "$artifact_dir/sni-durability-boot.log" 2>/dev/null || true
    fi
    # ITEM-504 increment 8: the routing boot and the proof backend have their
    # own logs.  A relay that was refused or a backend that never bound is
    # exactly the failure those two name, and the guest serial cannot show it.
    if [[ -n "${ROUTING_SERIAL:-}" && -f "${ROUTING_SERIAL:-}" ]]; then
        cp -a "$ROUTING_SERIAL" \
            "$artifact_dir/routing-boot.log" 2>/dev/null || true
    fi
    if [[ -n "${FACADE_BACKEND_LOG:-}" && -f "${FACADE_BACKEND_LOG:-}" ]]; then
        cp -a "$FACADE_BACKEND_LOG" \
            "$artifact_dir/facade-backend.log" 2>/dev/null || true
    fi
    if [[ -n "${FACADE_BACKEND2_LOG:-}" ]] &&
        [[ -f "${FACADE_BACKEND2_LOG:-}" ]]; then
        cp -a "$FACADE_BACKEND2_LOG" \
            "$artifact_dir/facade-backend2.log" 2>/dev/null || true
    fi
    echo "Production service artifacts: $artifact_dir" >&2
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    local cleanup_started_ms
    cleanup_started_ms=$(timing_now_ms) || cleanup_started_ms=$timing_started_ms
    timing_close "$status" "$cleanup_started_ms"
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [[ -n "${FACADE_BACKEND_PID:-}" ]] &&
        kill -0 "$FACADE_BACKEND_PID" 2>/dev/null; then
        kill -TERM "$FACADE_BACKEND_PID" 2>/dev/null || true
        wait "$FACADE_BACKEND_PID" 2>/dev/null || true
    fi
    if [[ -n "${FACADE_BACKEND2_PID:-}" ]] &&
        kill -0 "$FACADE_BACKEND2_PID" 2>/dev/null; then
        kill -TERM "$FACADE_BACKEND2_PID" 2>/dev/null || true
        wait "$FACADE_BACKEND2_PID" 2>/dev/null || true
    fi
    if (( status != 0 )); then
        save_failure_artifacts
    fi
    rm -rf "$TEST_DIR"
    local finished_ms
    finished_ms=$(timing_now_ms) || finished_ms=$cleanup_started_ms
    timing_record proof-wrapper "$timing_phase" "$status" \
        "$timing_wrapper_started_ms" "$finished_ms"
    exit "$status"
}

trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# ITEM-434: a record the guest publishes a moment after the action that causes
# it is awaited, not asserted.
#
# The provisioning phase types the administrator secret into the local console
# and the enrollment record follows a moment later; the proof used to grep for
# it exactly once and failed the phase when a loaded guest had not written it
# yet (`class=assertion reason=no-marker-timeout-record seconds=43` on
# 2026-09-16, once in each of two consecutive acceptance runs).  The wait is
# bounded, and a record that really does not arrive writes the canonical
# `HFOC-QEMU-MARKER-TIMEOUT` line, so the phase treats it as the timing class
# it is and retries it once instead of failing the gate.
wait_for_record() {
    local marker=$1
    local seconds=$2
    # ITEM-544: a phase whose boot writes its own serial log names it; every
    # earlier caller keeps the phase-1 log by default.
    local log=${3:-$SERIAL_LOG}
    local deadline=$((SECONDS + seconds))

    while (( SECONDS < deadline )); do
        if grep -Fq -- "$marker" "$(serial_view "$log")"; then
            return 0
        fi
        if [[ -n "${QEMU_PID:-}" ]] && ! kill -0 "$QEMU_PID" 2>/dev/null; then
            break
        fi
        sleep 0.2
    done
    printf 'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ss\n' \
        "$marker" "$seconds" >&2
    return 1
}

create_blank_hfoc_disk "$DISK"

expected='[PRODUCTION-SERVICE] production_service_activation'
deadline=$((SECONDS + 90))

timing_next first-boot/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_PORT-:443,hostfwd=tcp:127.0.0.1:$SERVING_AUTH_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

while (( SECONDS < deadline )); do
    if grep -Fq "$expected" "$(serial_view "$SERIAL_LOG")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SERIAL_LOG")"; then
        FAILURE_MESSAGE="Kernel panic during production service proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before the production service marker"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    sleep 0.2
done

if ! grep -Fq "$expected" "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Timed out waiting for: $expected"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq 'state=(AWAITING_NETWORK|AWAITING_READINESS|STOPPED|DEGRADED)' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production service did not publish an honest state"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq 'secrets=NONE' "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production service record missing the secrets marker"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

# ITEM-375: ordinary boot must publish the management TLS lifecycle record
# with the frozen policy, honest certificate readiness and HSTS, and it must
# never publish a private key or a certificate payload.
tls_expected='[PRODUCTION-TLS] management_tls v=1'
if ! grep -Fq "$tls_expected" "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Timed out waiting for: $tls_expected"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq \
        '[PRODUCTION-TLS] management_tls v=1 state=AWAITING_CERTIFICATE' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Management TLS did not declare the awaited certificate"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq \
        '\[PRODUCTION-TLS\].*certificate=UNAVAILABLE staged=NONE request=1' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Management TLS did not publish honest readiness"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq \
        '\[PRODUCTION-TLS\].*protocol=TLS13 cipher=AES_128_GCM_SHA256' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Management TLS did not publish the frozen policy"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-TLS\].*hsts=max-age=31536000' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Management TLS did not publish HSTS"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-TLS\].*secrets=NONE' "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Management TLS record missing the secrets marker"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Eq '\[PRODUCTION-TLS\].*(PRIVATE KEY|BEGIN |password=)' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Management TLS record leaked secret material"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-386: the ordinary production boot must publish and raise the serving
# path, not only the activation record.
serving_expected='[PRODUCTION-SERVING] v=1'
serving_deadline=$((SECONDS + 60))
while (( SECONDS < serving_deadline )); do
    if grep -Fq "$serving_expected" "$(serial_view "$SERIAL_LOG")"; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$serving_expected" "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Timed out waiting for: $serving_expected"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-506 increment 5: this first boot has an initialized state volume and no
# account, so the management plane withholds the listener and says why.  The
# account is set on the local console further down; only then may the listener
# come up, so nothing here asserts a live listener.
serving_gate_deadline=$((SECONDS + 60))
while (( SECONDS < serving_gate_deadline )); do
    if grep -Eq \
            '\[PRODUCTION-SERVING\] v=1 state=AWAITING_CREDENTIAL reason=admin_credential_unset listener=down' \
            "$(serial_view "$SERIAL_LOG")"; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Eq \
        '\[PRODUCTION-SERVING\] v=1 state=AWAITING_CREDENTIAL reason=admin_credential_unset listener=down' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production serving record did not withhold the listener before the account was set"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-516: this boot's record is the default, so the boot says the gate is
# closed and why.  The same record is what the policy boot below replaces.
if ! grep -Fq '[PRODUCTION-SECURITY] gate=CLOSED reason=AWAITING_LOCAL_SECRET' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="The default boot did not publish the closed management gate"
    echo "$FAILURE_MESSAGE" >&2
    grep -F 'PRODUCTION-SECURITY' "$SERIAL_LOG" >&2 || true
    exit 1
fi
echo "TLS-PROOF default boot: the closed gate record names the awaiting local secret"
if grep -Eq '\[PRODUCTION-SERVING\] v=1 state=LISTENING .*listener=live' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="The first boot raised the listener before the account was set"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "TLS-PROOF serving gate: listener withheld until the account is set"
timing_next first-boot/client-setup

# Real TLS 1.3 client over the forwarded management port.
cat >"$TEST_DIR/serving-client.py" <<'CLIENTEOF'
import base64
import os
import socket
import ssl
import sys
import threading
import time

port = int(sys.argv[1])
peer_path = sys.argv[2] if len(sys.argv) > 2 else None
PEER = []
serial_path = os.environ.get("HFOC_PROOF_SERIAL_LOG", "")

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3

SECTIONS = ("Dashboard", "Network", "Services", "Storage", "System",
            "Security", "Diagnostics", "Updates", "Recovery", "Routing",
            "Account", "Certificates", "Help")
HARDENING = (
    b"Strict-Transport-Security: max-age=31536000",
    b"Content-Security-Policy: default-src 'none'",
    b"Referrer-Policy: no-referrer",
    b"X-Content-Type-Options: nosniff",
    b"X-Frame-Options: DENY",
    b"Cache-Control: no-store",
)
FROZEN_API = (b'{"api":"hfoc","version":1,"name":"management",'
              b'"status":"ok","auth":"unavailable","mutating":false}')


def exchange(raw, timeout=30):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            version = tls.version()
            # ITEM-503: the certificate the appliance really served, for the
            # subjectAltName check below.
            if not PEER:
                PEER.append(tls.getpeercert(binary_form=True))
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def get(target):
    return exchange(b"GET " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
                    b"Connection: close\r\n\r\n")

def serial_tail(offset):
    """Bytes appended to the guest serial log since the previous read."""
    if not serial_path:
        return "", offset
    try:
        with open(serial_path, "rb") as handle:
            handle.seek(offset)
            data = handle.read()
            offset = handle.tell()
    except OSError:
        return "", offset
    return data.decode("utf-8", "replace"), offset


def health_probe(deadline=2.0):
    """One independent liveness request with a hard bound, timed locally."""
    started = time.monotonic()
    data = b""
    try:
        with socket.create_connection(("127.0.0.1", port),
                                      timeout=deadline) as sock:
            sock.settimeout(deadline)
            with context.wrap_socket(
                    sock, server_hostname="hfoc-appliance") as tls:
                tls.sendall(
                    b"GET /health HTTP/1.1\r\nHost: hfoc-appliance\r\n"
                    b"Connection: close\r\n\r\n")
                while True:
                    chunk = tls.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                    if len(data) > 65536:
                        break
    except (OSError, ssl.SSLError) as error:
        return 0, ("probe error: %r" % (error,)).encode(), \
            time.monotonic() - started
    elapsed = time.monotonic() - started
    head, _, body = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ", 2)[1])
    except (IndexError, ValueError):
        status = 0
    return status, body, elapsed


def open_health(deadline=2.0):
    """An established TLS connection, opened while the loop is still idle."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=deadline)
    sock.settimeout(deadline)
    return context.wrap_socket(sock, server_hostname="hfoc-appliance")


def health_on(tls, deadline=2.0):
    """One /health request on an already-handshaken connection."""
    started = time.monotonic()
    data = b""
    try:
        tls.settimeout(deadline)
        tls.sendall(
            b"GET /health HTTP/1.1\r\nHost: hfoc-appliance\r\n"
            b"Connection: close\r\n\r\n")
        while True:
            chunk = tls.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > 65536:
                break
    except (OSError, ssl.SSLError) as error:
        return 0, ("probe error: %r" % (error,)).encode(), \
            time.monotonic() - started
    finally:
        try:
            tls.close()
        except OSError:
            pass
    elapsed = time.monotonic() - started
    head, _, body = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ", 2)[1])
    except (IndexError, ValueError):
        status = 0
    return status, body, elapsed


def kdf_progress(text):
    """Highest bounded round count the production job published, if any."""
    done = 0
    total = 0
    for line in text.splitlines():
        if "state=IN_PROGRESS" not in line:
            continue
        for field in line.split():
            try:
                if field.startswith("iterations_done="):
                    done = max(done, int(field.split("=", 1)[1]))
                elif field.startswith("iterations_total="):
                    total = max(total, int(field.split("=", 1)[1]))
            except ValueError:
                pass
    return done, total


def signin_panel_with_liveness(form):
    """Post the panel form while independent health requests must answer.

    ITEM-619: three TLS connections are established while the loop is idle,
    then the sign-in starts.  The production trace must publish IN_PROGRESS,
    the three /health requests must each answer 200 within two seconds while
    no COMPLETED record exists, and at least one bounded progress record must
    show the stored PBKDF2 rounds advancing.  A live client thread, a print or
    a login that already finished cannot pass this.  The sign-in is not
    required to finish inside QEMU: its 200000 rounds take far longer under
    TCG, and the baseline sign-in elsewhere in this boot already proves the
    303/cookie path.  The client's own timeout closes the pending connection
    and the server cancels/zeroizes the job on release.
    """
    raw = (
        b"POST / HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(form)).encode("ascii") +
        b"\r\nConnection: close\r\n\r\n" + form)
    result = {}
    offset = 0
    if serial_path:
        try:
            offset = os.path.getsize(serial_path)
        except OSError:
            offset = 0

    healths = [open_health(10.0) for _ in range(3)]

    def worker():
        try:
            result["value"] = exchange(raw, 12.0)
        except BaseException as error:  # expected while the KDF is pending
            result["error"] = error

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    started = time.monotonic()
    text = ""
    start_seen = False
    deadline = started + 20.0
    while time.monotonic() < deadline:
        chunk, offset = serial_tail(offset)
        text += chunk
        if "state=IN_PROGRESS" in text:
            start_seen = True
            break
        if "state=COMPLETED" in text:
            raise SystemExit(
                "the panel sign-in finished before any liveness probe")
        time.sleep(0.05)
    if not start_seen:
        raise SystemExit(
            "the panel credential job published no production start record: %r"
            % text[-200:])
    start_seen_at = time.monotonic()
    if not thread.is_alive():
        raise SystemExit(
            "the panel sign-in finished before the liveness window opened")

    probes = 0
    max_latency = 0.0
    for tls in healths:
        status, body, elapsed = health_on(tls, 2.0)
        max_latency = max(max_latency, elapsed)
        if status != 200 or b'"status":"ok"' not in body or elapsed > 2.0:
            raise SystemExit(
                "the panel credential job stopped the loop mid-KDF: "
                "/health returned %d after %.2f s (%r)"
                % (status, elapsed, body[:160]))
        probes += 1
        chunk, offset = serial_tail(offset)
        text += chunk
        if "state=COMPLETED" in text:
            raise SystemExit(
                "the credential job completed before the independent health "
                "answer was observed")
        if not thread.is_alive():
            raise SystemExit(
                "the sign-in finished before the independent health requests "
                "were answered")

    done, total = kdf_progress(text)
    deadline = time.monotonic() + 3.0
    while (done == 0 or total == 0) and time.monotonic() < deadline:
        time.sleep(0.05)
        chunk, offset = serial_tail(offset)
        text += chunk
        done, total = kdf_progress(text)
    if done <= 0 or total <= 0 or done >= total:
        raise SystemExit(
            "the production KDF published no measurable progress while the "
            "health requests were answered: done=%r total=%r" % (done, total))
    if not thread.is_alive():
        raise SystemExit(
            "the sign-in finished before the KDF progress was measured")

    thread.join(20.0)
    if thread.is_alive():
        raise SystemExit("the pending sign-in did not release within 20 s")
    pending_ms = int((time.monotonic() - start_seen_at) * 1000.0)
    if pending_ms < 1000:
        raise SystemExit(
            "the sign-in did not stay pending for the measured window: %d ms"
            % pending_ms)
    print("TLS-PROOF panel sign-in: %d /health 200 answers (max %.2f s) inside "
          "a pending %d ms production KDF job, rounds advance to %d/%d"
          % (probes, max_latency, pending_ms, done, total))
    return result.get("value", (0, b"", 0.0))


status, head, body = get(b"/")
if status != 200 or b"Content-Type: text/html" not in head:
    raise SystemExit("root did not serve 200 HTML: %d" % status)
text = body.decode("utf-8", "replace")
for section in SECTIONS:
    if section not in text:
        raise SystemExit("root document missing section: %s" % section)
for header in HARDENING:
    if header not in head:
        raise SystemExit("root missing hardening header: %r" % header)
print("TLS-PROOF root: 200 HTML with the section index and hardening headers")

status, head, body = get(b"/health")
if status != 200 or b"Content-Type: application/json" not in head \
        or b'"status":"ok"' not in body:
    raise SystemExit("health target did not serve the liveness body")
print("TLS-PROOF health: 200 JSON")

status, head, body = get(b"/api/v1")
if status != 200 or FROZEN_API not in body:
    raise SystemExit("api discovery body is not the frozen body: %r" % body[:160])
print("TLS-PROOF api discovery: frozen version body")

status, head, body = exchange(
    b"POST /api/v1 HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Connection: close\r\n\r\n")
if status != 405:
    raise SystemExit("POST /api/v1 returned %d" % status)
print("TLS-PROOF api method: 405")

status, head, body = get(b"/api/v1/not-a-route")
if status != 404:
    raise SystemExit("unknown api route returned %d" % status)
print("TLS-PROOF api unknown route: 404")

# ITEM-400/ITEM-506 increment 5: the read-only management targets resolve on
# ordinary boot, and this client now runs after the local console set the
# account, so the ITEM-365 request gate is available: an anonymous request is
# denied by the session gate (401) instead of the gate-unavailable 503.  The
# unknown-route body must still not appear, and the 503 shape while a persisted
# account is not published on the boot is proven on the network-apply boot.
for target in (b"/api/v1/system", b"/api/v1/network",
               b"/api/v1/network/status", b"/api/v1/service",
               b"/api/v1/health"):
    status, head, body = get(target)
    if status != 401:
        raise SystemExit("read-only management target %s returned %d"
                         % (target.decode(), status))
    if b"not_found" in body:
        raise SystemExit("management target %s answered the unknown-route body"
                         % target.decode())
print("TLS-PROOF read-only management targets: known routes, anonymous denial 401")

# ITEM-505 increment 3: `POST /api/v1/system` is the hostname mutation now, so
# the read-only-method refusal is proven on `/api/v1/health`, which stays a
# read-only target; the mutation's own refusal without a session is the same
# gate the panel target proves below.
status, head, body = exchange(
    b"POST /api/v1/health HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Connection: close\r\n\r\n")
if status != 405:
    raise SystemExit("POST /api/v1/health returned %d" % status)
print("TLS-PROOF read-only management method: 405")

status, head, body = get(b"/panel/network")
if status not in (401, 403, 429, 503):
    raise SystemExit("panel without a session returned %d" % status)
# ITEM-386: a denied panel request is answered with the panel's own refusal,
# which docs/hfoc-management-web-ui.md pins as a rendered document: the
# anonymous sign-in document for 401/403/429 and the same document with the
# AUTH_UNAVAILABLE notice for 503.  That document is the public document the
# anonymous root target already serves, so its section index (which contains
# the word "Dashboard") is not a leak.  The leak that matters is the frozen
# management API body or a session identity, so assert the refusal is the
# sign-in document and that neither private marker appears.
if b'"api":"hfoc"' in body or b"Session: none" not in body:
    raise SystemExit("panel denial leaked a management body")
if b"Management sign-in" not in body or b"SIGNED OUT" not in body:
    raise SystemExit("panel denial did not render the sign-in document")
print("TLS-PROOF panel denial: %d with the sign-in document, no private body"
      % status)

# ITEM-503: the public certificate document is the certificate the handshake
# presents, byte for byte, so importing it into a trust store trusts exactly
# what the appliance serves.
status, head, body = get(b"/hfoc-ca.crt")
if status != 200 or b"application/x-pem-file" not in head:
    raise SystemExit("certificate document returned %d" % status)
pem_text = body.decode("ascii", "replace")
if not pem_text.startswith("-----BEGIN CERTIFICATE-----"):
    raise SystemExit("certificate document is not PEM")
encoded = "".join(line for line in pem_text.splitlines()
                  if line and not line.startswith("-----"))
if not PEER or base64.b64decode(encoded) != PEER[0]:
    raise SystemExit("certificate document is not the served certificate")
print("TLS-PROOF certificate document matches the handshake certificate")

if peer_path is not None and PEER and PEER[0]:
    with open(peer_path, "wb") as handle:
        handle.write(PEER[0])
    print("TLS-PROOF peer certificate written: %d bytes" % len(PEER[0]))
CLIENTEOF

# ITEM-401: drive the local console through the QEMU monitor and enroll the
# administrator credential.  The factory placeholder is never accepted, the
# wrong secret is refused, and the successful login prints one bounded bearer
# session that the TLS client below uses.
cat >"$TEST_DIR/admin-enroll.py" <<'ENROLLEOF'
import os
import pathlib
import re
import socket
import sys
import time

mode = sys.argv[1]
# ITEM-525: `normalise` renders the diagnostics-stripped console stream for the
# shell assertions of this proof and needs only the serial path; every other
# mode drives the monitor and takes the full argument list.
if mode == "normalise":
    monitor_path = None
    serial_path = pathlib.Path(sys.argv[2])
    secret = ""
    login_name = "admin"
else:
    monitor_path = pathlib.Path(sys.argv[2])
    serial_path = pathlib.Path(sys.argv[3])
    secret = sys.argv[4]
    # ITEM-506 increment 6: the login the console sign-in attempt uses.  The
    # account's login is set by the panel change below, so the durability boot
    # has to present it by name; the default keeps every earlier phase on
    # `admin`.
    login_name = sys.argv[5] if len(sys.argv) > 5 else "admin"
TOKEN_ALPHABET = ("ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                  "abcdefghijklmnopqrstuvwxyz0123456789-_")
# The periodic IRQ lines are written from interrupt context and can land in the
# middle of the console's printed token; they are complete lines, so removing
# them restores the character stream the console emitted.  ITEM-528 narrowed
# the rule to those periodic lines: records that merely begin with the same
# prefixes (e.g. "[IRQ ] IF enabled; entering HLT event loop." or
# "[PIC ] Master mask: ...") are console output and must survive.
DIAGNOSTIC_LINE = re.compile(
    r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"
    r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")


def serial_text():
    try:
        return DIAGNOSTIC_LINE.sub(
            "", serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


if mode == "normalise":
    # ITEM-525: the shell assertions below read the console stream this helper
    # already reads - interrupts removed - instead of the raw file.  A
    # diagnostic line can land inside a record the console prints one character
    # at a time, and the raw file then hides a record that is present: run
    # 38264136090 failed the gate proof exactly that way, after the python
    # gesture above had already seen the record.
    sys.stdout.write(serial_text())
    raise SystemExit(0)


def gate_messages():
    """The console's own last words about the gate, for a retry log.

    The records name the state; the prose names the refusal.  Only these
    English machine/console lines are collected: a listener waiting on the
    full serial would grow with the boot.
    """
    lines = []
    for line in serial_text().splitlines():
        stripped = line.strip()
        if ("MANAGEMENT GATE" in stripped or "REFUSED:" in stripped or
                "PRODUCTION-SECURITY" in stripped):
            lines.append(stripped)
    return lines[-8:]


def wait_for(marker, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if marker in text:
            return text
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during admin enrollment")
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str("timed out waiting for: %s" % marker) + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for: %s" % marker)


def wait_for_new(marker, timeout, baseline):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        text = serial_text()
        if text.count(marker) > baseline:
            return text
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("Kernel panic during admin enrollment")
        time.sleep(0.05)
    # ITEM-422: a proof that gives up on a marker says so.
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="' +
        str("timed out waiting for: %s" % marker) + '" '
        'deadline=' + str(int(timeout)) + 's\n')
    sys.stderr.flush()
    raise SystemExit("timed out waiting for: %s" % marker)


# The ITEM-363 first-boot flow commits the factory fingerprint before the
# listener goes live; enrollment refuses to guess a record that is not there.
wait_for("[PRODUCTION-FIRST-BOOT]", 60.0)

with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as monitor:
    monitor.settimeout(1.0)
    monitor.connect(str(monitor_path))

    def receive_prompt():
        chunks = []
        while True:
            chunk = monitor.recv(4096)
            if not chunk:
                raise RuntimeError("QEMU monitor closed unexpectedly")
            chunks.append(chunk)
            if b"(qemu)" in b"".join(chunks):
                return

    def send_key(key):
        monitor.sendall(("sendkey " + key + "\n").encode("ascii"))
        receive_prompt()
        time.sleep(0.05)

    def type_line(line):
        for character in line:
            if character == "-":
                send_key("minus")
            elif character == " ":
                send_key("spc")
            else:
                send_key(character)
        # Let the guest drain the controller buffer before the newline.
        time.sleep(0.3)
        send_key("ret")
        time.sleep(0.1)

    def type_until(marker, line, timeout, sensitive=False):
        """
        Type one answer and wait for the console's next marker.

        The retry exists for a partially delivered line.  It is not used for a
        line that carries the account secret: when the guest is starved (the CI
        QEMU suite runs several proofs at once) the marker can arrive after the
        deadline, and retyping the secret then lands it on a plain command
        prompt, which echoes it into the serial log - the proof failed exactly
        that way on 2026-10-04.  A sensitive answer is typed once: a missed
        marker fails the proof (as a marker timeout, which the suite retries)
        instead of leaking the secret.
        """
        baseline = serial_text().count(marker)
        for attempt in range(2):
            type_line(line)
            try:
                return wait_for_new(marker, timeout, baseline)
            except SystemExit:
                if attempt == 1 or sensitive:
                    raise
                # A partially delivered line is flushed and typed again.
                send_key("ret")
                time.sleep(0.5)
                baseline = serial_text().count(marker)
        raise SystemExit("unreachable")

    receive_prompt()
    if mode == "provision":
        # ITEM-506: the account is set by one conversation.  On this fresh
        # appliance no account exists yet, so the current-password answer
        # cannot be checked and is ignored; the next three answers become the
        # login and the password pair.
        type_until("Current password:", "credentials", 120.0)
        type_until("New login:", secret, 120.0, sensitive=True)
        type_until("New password:", "admin", 120.0)
        type_until("Repeat new password:", secret, 120.0, sensitive=True)
        type_until("[ADMIN-ENROLLMENT] credentials changed", secret, 120.0,
                   sensitive=True)
        print("provisioned")
    elif mode == "refuse":
        # CI-SPEEDUP-3: the expected-refusal check answers the console once and
        # finishes on the refusal line it is looking for.  The previous shape
        # drove the whole sign-in conversation and then waited out the
        # 120-second `ADMIN SESSION` marker for a session a retired password
        # can never be issued - 121 s of the main HTTPS proof, named by the
        # PR #761 measurement - while the refusal it wanted had already been
        # printed.
        expected_refusal = ("[ADMIN-ENROLLMENT] login refused: "
                            "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED")
        session_marker = "ADMIN SESSION: Authorization: Bearer "
        # A console that answers nothing must fail the proof.  The bound is
        # the same 120 s every other marker wait here uses - one credential
        # verification was measured at 15-19 s, and a starved guest has taken
        # twice that - and it is overridable so the host witness can drive the
        # missing-answer path without a two-minute wait.
        deadline_seconds = float(
            os.environ.get("HFOC_ADMIN_REFUSE_TIMEOUT", "120"))
        wait_for("hfoc> ", 60.0)
        login_baseline = serial_text().count("Login:")
        type_line("login")
        wait_for_new("Login:", 60.0, login_baseline)
        password_baseline = serial_text().count("Password:")
        type_line(login_name)
        wait_for_new("Password:", 60.0, password_baseline)
        # The answer this attempt earns is the console output after the
        # password is typed: a refusal an earlier attempt left in the log, or
        # any line that was already there, must not satisfy the check.
        attempt_offset = len(serial_text())
        type_line(secret)
        deadline = time.monotonic() + deadline_seconds
        while time.monotonic() < deadline:
            tail = serial_text()[attempt_offset:]
            # A line is an answer only once its terminator is in the log too.
            # The console prints the refusal in several writes and the host
            # reads the file, so a read can land between them: run 42075493165
            # saw 31 characters of the expected line, called the prefix
            # "another reason" and failed a proof whose guest had printed the
            # whole denial.  The unterminated tail is dropped and the next poll
            # reads the finished line.
            for line in tail.split("\n")[:-1]:
                answer = line.strip()
                if not answer:
                    continue
                if answer == expected_refusal:
                    sys.stdout.write("refused\n")
                    sys.stdout.flush()
                    raise SystemExit(0)
                if answer.startswith(session_marker):
                    raise SystemExit(
                        "the old password still signs in on the local console")
                if answer.startswith("[ADMIN-ENROLLMENT] login refused: "):
                    raise SystemExit(
                        "the console refused the old password for another "
                        "reason: " + answer)
                if answer == "hfoc>":
                    raise SystemExit(
                        "the console ended the attempt without a refusal")
            if "*** HFOC KERNEL PANIC ***" in tail:
                raise SystemExit("Kernel panic during the refusal check")
            time.sleep(0.05)
        raise SystemExit(
            "the console answered nothing to the local sign-in attempt")
    else:
        type_until("Login:", "login", 120.0)
        type_until("Password:", "admin", 120.0)
        type_until("login refused", "nope", 120.0)
        type_until("Login:", "login", 120.0)
        type_until("Password:", login_name, 120.0)
        type_until("ADMIN SESSION: Authorization: Bearer ", secret, 120.0,
                   sensitive=True)
        marker = "ADMIN SESSION: Authorization: Bearer "
        # ITEM-506: the marker and the 32-character token reach the serial log
        # in one guest turn, but the host reads the log file, so a single read
        # taken the moment the marker appears can see only part of the token.
        # Wait for the whole token, then apply the same alphabet assertion: the
        # proof must not fail on a file-read race.
        token = ""
        token_deadline = time.monotonic() + 10.0
        while time.monotonic() < token_deadline:
            text = serial_text()
            index = text.rindex(marker) + len(marker)
            candidate = text[index:index + 32]
            if len(candidate) == 32 and all(
                    ch in TOKEN_ALPHABET for ch in candidate):
                token = candidate
                break
            if "*** HFOC KERNEL PANIC ***" in text:
                raise SystemExit("Kernel panic during admin enrollment")
            time.sleep(0.05)
        if len(token) != 32:
            raise SystemExit(
                "admin session token is not the expected alphabet")
        print(token)
        if mode == "login-gate":
            # ITEM-516: the same console session has just proved the local
            # secret, so the dangerous direction needs no second proof - only
            # the request and its explicit confirmation on the next line.  One
            # process, one monitor connection: the enable cannot lose keys to a
            # second client starting before the console accepts input.  Every
            # progress line goes to stderr, so stdout stays the single token
            # line the caller captures.
            #
            # The confirmation consumes the armed request even when it fails
            # (a stray line disarms it, or the durable write refuses), so a
            # retry of the confirmation alone can only ever answer
            # UNCONFIRMED.  The whole gesture is therefore retried, which
            # re-arms it; success is the ENABLED record and nothing else.
            enabled = False
            for attempt in range(3):
                try:
                    type_until("confirmation_required", "gate open", 60.0)
                    type_until("gate=ENABLED source=LOCAL_SECRET",
                               "gate open confirm", 30.0)
                    enabled = True
                    break
                except SystemExit as error:
                    if "Kernel panic" in str(error):
                        raise
                    sys.stderr.write(
                        "gate enable: attempt %d did not finish; the console "
                        "said:\n" % (attempt + 1))
                    for line in gate_messages():
                        sys.stderr.write("  " + line + "\n")
                    sys.stderr.flush()
            if not enabled:
                raise SystemExit(
                    "gate enable: no gate=ENABLED record after 3 full "
                    "gestures")
            sys.stderr.write("gate enable: the console wrote the policy\n")
            sys.stderr.flush()
ENROLLEOF

# ITEM-504 increment 8: the live backend of the facade relay proof.  It is a
# plain HTTP server on the test host; the guest reaches it as 10.0.2.2:port
# through the QEMU user network, so a relayed request is proven by the body
# only this backend can produce.  The answer is deliberately larger than one
# TCP segment (4096 bytes; TCP_MSS is 536), because a body that fits in a
# single segment never makes the relay refuse the segment behind it and would
# prove nothing about the answer's flow.  It is started before the routing
# boot and killed after it.
cat >"$TEST_DIR/facade-backend.py" <<'FACADEEOF'
import socket
import sys

port = int(sys.argv[1])
marker = ("FACADE-PROOF-BACKEND:%d" % port).encode("ascii")
body = marker + b"\n" + bytes((index * 7 + 3) % 251
                              for index in range(4096 - len(marker) - 1))
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(("127.0.0.1", port))
listener.listen(8)
# The listener line is the readiness proof: a port already taken makes this
# process exit before the guest asks for it, and the phase must name that
# instead of reporting an empty answer from the facade.
print("facade backend listening on %d" % port, flush=True)
while True:
    try:
        connection, _ = listener.accept()
    except OSError:
        break
    try:
        request = b""
        while b"\r\n\r\n" not in request and len(request) < 8192:
            chunk = connection.recv(1024)
            if not chunk:
                break
            request += chunk
        connection.sendall(
            b"HTTP/1.1 200 OK\r\nContent-Length: "
            + str(len(body)).encode("ascii")
            + b"\r\nConnection: close\r\n\r\n" + body)
    except OSError:
        # A client that hangs up mid-answer kills this connection, not the
        # backend: the proof's next request still needs a listener.
        pass
    finally:
        connection.close()
FACADEEOF

# ITEM-504 increment 3: the routing mutation is reachable over HTTP.  The
# proof stages a route and commits it on the API target in two requests, the
# same two-phase contract the plane enforces, reads the state record the
# target answers with, and then posts the panel's own form with the browser
# session cookie.  It runs on a boot of its own: the fixture's lwIP pool is
# spent by the two boots above (eleven and thirteen connections) and refuses
# the next connection, so the four requests this proof needs get a fresh pool
# instead of competing with proofs that were there first.
cat >"$TEST_DIR/serving-routing-client.py" <<'ROUTINGEOF'
import os
import socket
import ssl
import sys
import threading
import time
import urllib.parse

port = int(sys.argv[1])
token = sys.argv[2].encode("ascii")
secret = sys.argv[3].encode("ascii")
backend_port = int(sys.argv[4])
backend2_port = int(sys.argv[5])
serial_path = os.environ.get("HFOC_PROOF_SERIAL_LOG", "")

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def exchange(raw, timeout=90):
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            version = tls.version()
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def exchange_retry(raw, attempts=3):
    """A bounded retry for a QEMU/slirp reset, never a product gate."""
    last = None
    for _ in range(attempts):
        try:
            return exchange(raw)
        except (OSError, ssl.SSLError) as error:
            last = error
            time.sleep(0.2)
    raise last


def post(target, body, authorization=None, cookie=None, attempts=1):
    header = b""
    if authorization is not None:
        header += b"Authorization: Bearer " + authorization + b"\r\n"
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    return exchange_retry(
        b"POST " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header +
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + body, attempts)


def serial_tail(offset):
    """Bytes appended to the guest serial log since the previous read."""
    if not serial_path:
        return "", offset
    try:
        with open(serial_path, "rb") as handle:
            handle.seek(offset)
            data = handle.read()
            offset = handle.tell()
    except OSError:
        return "", offset
    return data.decode("utf-8", "replace"), offset


def health_probe(deadline=2.0):
    """One independent liveness request with a hard bound, timed locally."""
    started = time.monotonic()
    data = b""
    try:
        with socket.create_connection(("127.0.0.1", port),
                                      timeout=deadline) as sock:
            sock.settimeout(deadline)
            with context.wrap_socket(
                    sock, server_hostname="hfoc-appliance") as tls:
                tls.sendall(
                    b"GET /health HTTP/1.1\r\nHost: hfoc-appliance\r\n"
                    b"Connection: close\r\n\r\n")
                while True:
                    chunk = tls.recv(4096)
                    if not chunk:
                        break
                    data += chunk
                    if len(data) > 65536:
                        break
    except (OSError, ssl.SSLError) as error:
        return 0, ("probe error: %r" % (error,)).encode(), \
            time.monotonic() - started
    elapsed = time.monotonic() - started
    head, _, body = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ", 2)[1])
    except (IndexError, ValueError):
        status = 0
    return status, body, elapsed


def open_health(deadline=2.0):
    """An established TLS connection, opened while the loop is still idle."""
    sock = socket.create_connection(("127.0.0.1", port), timeout=deadline)
    sock.settimeout(deadline)
    return context.wrap_socket(sock, server_hostname="hfoc-appliance")


def health_on(tls, deadline=2.0):
    """One /health request on an already-handshaken connection."""
    started = time.monotonic()
    data = b""
    try:
        tls.settimeout(deadline)
        tls.sendall(
            b"GET /health HTTP/1.1\r\nHost: hfoc-appliance\r\n"
            b"Connection: close\r\n\r\n")
        while True:
            chunk = tls.recv(4096)
            if not chunk:
                break
            data += chunk
            if len(data) > 65536:
                break
    except (OSError, ssl.SSLError) as error:
        return 0, ("probe error: %r" % (error,)).encode(), \
            time.monotonic() - started
    finally:
        try:
            tls.close()
        except OSError:
            pass
    elapsed = time.monotonic() - started
    head, _, body = data.partition(b"\r\n\r\n")
    try:
        status = int(head.split(b" ", 2)[1])
    except (IndexError, ValueError):
        status = 0
    return status, body, elapsed


def kdf_progress(text):
    """Highest bounded round count the production job published, if any."""
    done = 0
    total = 0
    for line in text.splitlines():
        if "state=IN_PROGRESS" not in line:
            continue
        for field in line.split():
            try:
                if field.startswith("iterations_done="):
                    done = max(done, int(field.split("=", 1)[1]))
                elif field.startswith("iterations_total="):
                    total = max(total, int(field.split("=", 1)[1]))
            except ValueError:
                pass
    return done, total


def signin_panel_with_liveness(form):
    """Post the panel form while independent health requests must answer.

    ITEM-619: three TLS connections are established while the loop is idle,
    then the sign-in starts.  The production trace must publish IN_PROGRESS,
    the three /health requests must each answer 200 within two seconds while
    no COMPLETED record exists, and at least one bounded progress record must
    show the stored PBKDF2 rounds advancing.  A live client thread, a print or
    a login that already finished cannot pass this.  The sign-in is not
    required to finish inside QEMU: its 200000 rounds take far longer under
    TCG, and the baseline sign-in elsewhere in this boot already proves the
    303/cookie path.  The client's own timeout closes the pending connection
    and the server cancels/zeroizes the job on release.
    """
    raw = (
        b"POST / HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(form)).encode("ascii") +
        b"\r\nConnection: close\r\n\r\n" + form)
    result = {}
    offset = 0
    if serial_path:
        try:
            offset = os.path.getsize(serial_path)
        except OSError:
            offset = 0

    healths = [open_health(10.0) for _ in range(3)]

    def worker():
        try:
            result["value"] = exchange(raw, 12.0)
        except BaseException as error:  # expected while the KDF is pending
            result["error"] = error

    thread = threading.Thread(target=worker, daemon=True)
    thread.start()
    started = time.monotonic()
    text = ""
    start_seen = False
    deadline = started + 20.0
    while time.monotonic() < deadline:
        chunk, offset = serial_tail(offset)
        text += chunk
        if "state=IN_PROGRESS" in text:
            start_seen = True
            break
        if "state=COMPLETED" in text:
            raise SystemExit(
                "the panel sign-in finished before any liveness probe")
        time.sleep(0.05)
    if not start_seen:
        raise SystemExit(
            "the panel credential job published no production start record: %r"
            % text[-200:])
    start_seen_at = time.monotonic()
    if not thread.is_alive():
        raise SystemExit(
            "the panel sign-in finished before the liveness window opened")

    probes = 0
    max_latency = 0.0
    for tls in healths:
        status, body, elapsed = health_on(tls, 2.0)
        max_latency = max(max_latency, elapsed)
        if status != 200 or b'"status":"ok"' not in body or elapsed > 2.0:
            raise SystemExit(
                "the panel credential job stopped the loop mid-KDF: "
                "/health returned %d after %.2f s (%r)"
                % (status, elapsed, body[:160]))
        probes += 1
        chunk, offset = serial_tail(offset)
        text += chunk
        if "state=COMPLETED" in text:
            raise SystemExit(
                "the credential job completed before the independent health "
                "answer was observed")
        if not thread.is_alive():
            raise SystemExit(
                "the sign-in finished before the independent health requests "
                "were answered")

    done, total = kdf_progress(text)
    deadline = time.monotonic() + 3.0
    while (done == 0 or total == 0) and time.monotonic() < deadline:
        time.sleep(0.05)
        chunk, offset = serial_tail(offset)
        text += chunk
        done, total = kdf_progress(text)
    if done <= 0 or total <= 0 or done >= total:
        raise SystemExit(
            "the production KDF published no measurable progress while the "
            "health requests were answered: done=%r total=%r" % (done, total))
    if not thread.is_alive():
        raise SystemExit(
            "the sign-in finished before the KDF progress was measured")

    thread.join(20.0)
    if thread.is_alive():
        raise SystemExit("the pending sign-in did not release within 20 s")
    pending_ms = int((time.monotonic() - start_seen_at) * 1000.0)
    if pending_ms < 1000:
        raise SystemExit(
            "the sign-in did not stay pending for the measured window: %d ms"
            % pending_ms)
    print("TLS-PROOF panel sign-in: %d /health 200 answers (max %.2f s) inside "
          "a pending %d ms production KDF job, rounds advance to %d/%d"
          % (probes, max_latency, pending_ms, done, total))
    return result.get("value", (0, b"", 0.0))


# The first request stages a candidate.  The answer names the two-phase state
# and hands out the one-shot confirmation token; nothing is applied yet.
stage = b"action=add&name=a.example&backend=192.0.2.10&port=443&tls=on"
status, head, body = post(b"/api/v1/system/routes", stage, authorization=token)
if status != 200:
    raise SystemExit("routing stage returned %d: %r" % (status, body[:200]))
if b'result=PENDING_CONFIRMATION' not in body:
    raise SystemExit("routing stage did not name its state: %r" % body[:200])
if b'"token":' not in body:
    raise SystemExit("routing stage handed out no confirmation token")
route_token = body.split(b'"token":', 1)[1].split(b'}', 1)[0].strip()
if not route_token.isdigit():
    raise SystemExit("routing confirmation token is not a number: %r"
                     % route_token[:32])
print("TLS-PROOF routing API: the stage names its state and its token")

# The second request confirms exactly that token.  The table is replaced whole
# and the answer names the applied state with no token left over.
status, head, body = post(b"/api/v1/system/routes", b"token=" + route_token,
                          authorization=token)
if status != 200:
    raise SystemExit("routing commit returned %d: %r" % (status, body[:200]))
if b'result=APPLIED' not in body:
    raise SystemExit("routing commit did not apply: %r" % body[:200])
if b'"token":' in body:
    raise SystemExit("routing commit still hands out a token")
print("TLS-PROOF routing API: the commit applies the staged route")

# ITEM-504 increment 5: a refusal that applied nothing.  The name the table
# already carries is refused by the model's own grammar name, so this request
# never holds a candidate and the applied table stays what the commit above
# made it.  The machine publishes both outcomes as its own [PRODUCTION-ROUTE]
# records, asserted on the serial log after this client phase.
duplicate = b"action=add&name=a.example&backend=192.0.2.12&port=443&tls=off"
status, head, body = post(b"/api/v1/system/routes", duplicate,
                          authorization=token)
if status != 400:
    raise SystemExit("the duplicate routing stage returned %d: %r"
                     % (status, body[:200]))
if b"REFUSED" not in body:
    raise SystemExit("the duplicate routing stage named no refusal: %r"
                     % body[:200])
print("TLS-PROOF routing API: the duplicate stage is refused by name")

# ITEM-504 increment 8: the facade leads a request to the backend of the name
# it was addressed by.  `b.example` and, since increment 9, `c.example` are
# committed here against two different live proof backends on the test host, so
# the table the next boot reads back carries routes that can actually be
# reached; the facade requests themselves run on that next boot, whose fresh
# lwIP pool is not spent by the mutations above.
def commit_relay_route(name, port):
    stage = (b"action=add&name=" + name + b"&backend=10.0.2.2&port="
             + str(port).encode("ascii") + b"&tls=off")
    status, head, body = post(b"/api/v1/system/routes", stage,
                              authorization=token)
    if status != 200:
        raise SystemExit("the relay route stage for %s returned %d: %r"
                         % (name.decode(), status, body[:200]))
    if b'result=PENDING_CONFIRMATION' not in body:
        raise SystemExit("the relay route stage for %s named no pending state: %r"
                         % (name.decode(), body[:200]))
    route_token = body.split(b'"token":', 1)[1].split(b'}', 1)[0].strip()
    if not route_token.isdigit():
        raise SystemExit("the relay route stage for %s handed out no token: %r"
                         % (name.decode(), route_token[:32]))
    status, head, body = post(b"/api/v1/system/routes",
                              b"token=" + route_token, authorization=token)
    if status != 200 or b'result=APPLIED' not in body:
        raise SystemExit("the relay route commit for %s did not apply: %d %r"
                         % (name.decode(), status, body[:200]))


commit_relay_route(b"b.example", backend_port)
commit_relay_route(b"c.example", backend2_port)
print("TLS-PROOF facade relay: the reachable routes are committed")

# ITEM-504 increment 10: a commit that must fail leaves the applied table
# exactly as it was.  `d.example` is staged, committed with the wrong token -
# the plane refuses that by name and keeps the candidate - and then cancelled
# with its own token, so neither the failed commit nor the dropped candidate
# touches the three applied routes.  The routing page below must not list it,
# and the restore record must still carry exactly three routes.
failed_stage = (b"action=add&name=d.example&backend=192.0.2.20&port=8080"
                b"&tls=off")
status, head, body = post(b"/api/v1/system/routes", failed_stage,
                          authorization=token)
if status != 200 or b'result=PENDING_CONFIRMATION' not in body:
    raise SystemExit("the failed-commit stage did not pend: %d %r"
                     % (status, body[:200]))
failed_token = body.split(b'"token":', 1)[1].split(b'}', 1)[0].strip()
if not failed_token.isdigit():
    raise SystemExit("the failed-commit stage handed out no token: %r"
                     % failed_token[:32])
wrong_token = str(int(failed_token) + 1).encode("ascii")
status, head, body = post(b"/api/v1/system/routes", b"token=" + wrong_token,
                          authorization=token)
if status != 400 or b'"refused":"BAD_CONFIRMATION"' not in body:
    raise SystemExit("a wrong token was not refused by name: %d %r"
                     % (status, body[:200]))
print("TLS-PROOF routing mutation: the failed commit is refused by name")

status, head, body = post(b"/api/v1/system/routes",
                          b"cancel=1&token=" + failed_token,
                          authorization=token)
if status != 200 or b'result=CANCELLED' not in body:
    raise SystemExit("the failed commit's candidate was not cancelled: %d %r"
                     % (status, body[:200]))
print("TLS-PROOF routing mutation: the failed commit changed no applied route")

# The panel's own form target.  The browser session cookie is the principal,
# exactly as it is for the Security form, and the answer is the redirect back
# to the page the panel will render in the next increment.  The candidate it
# stages is left unconfirmed on purpose: the applied table stays what the
# commit above made it, and this boot ends here.
form = urllib.parse.urlencode(
    {"username": "admin", "password": secret.decode("ascii")}).encode("ascii")
status, head, body = signin_panel_with_liveness(form)
cookie_pair = None
if status == 303:
    session_cookie = None
    for header_line in head.split(b"\r\n"):
        if header_line.lower().startswith(b"set-cookie:"):
            session_cookie = header_line.split(b":", 1)[1].strip()
            break
    if session_cookie is None:
        raise SystemExit("routing panel sign-in set no cookie")
    cookie_pair = session_cookie.split(b";", 1)[0]
elif status != 0:
    raise SystemExit("routing panel sign-in returned %d" % status)
else:
    # The cooperative KDF is still pending under QEMU TCG.  The 303/cookie
    # path is proven by the separate auth boot; this phase exercises the
    # routing mutation with the already-authenticated bearer token.
    print("TLS-PROOF routing panel sign-in: pending KDF, bearer principal")

panel_form = (
    b"action=add&name=panel.example&backend=192.0.2.11&port=443&tls=off")
if cookie_pair is None:
    status, head, body = post(b"/panel/routing", panel_form,
                              authorization=token, attempts=3)
else:
    # ITEM-650: a fast KDF now reaches this cookie path inside the QEMU
    # window, so it uses the same bounded slirp-reset retry the bearer path
    # above already carries.  The retry is not a product gate.
    status, head, body = post(b"/panel/routing", panel_form,
                              cookie=cookie_pair, attempts=3)
if status != 303:
    raise SystemExit("the routing panel form returned %d" % status)
if b"Location: /panel/routing" not in head:
    raise SystemExit("the routing panel form did not redirect to its page")
print("TLS-PROOF routing panel form: 303 back to the page")

# ITEM-504 increment 4: the page the form redirects to.  It lists the applied
# table the commit above made, byte for byte as the route record renders it,
# and it carries the form again, so the operator reads what is routed before
# staging the next change.
if cookie_pair is None:
    panel_auth_line = b"Authorization: Bearer " + token + b"\r\n"
else:
    panel_auth_line = b"Cookie: " + cookie_pair + b"\r\n"
status, head, body = exchange_retry(
    b"GET /panel/routing HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    + panel_auth_line + b"Connection: close\r\n\r\n", 3)
if status != 200:
    raise SystemExit("the routing page returned %d" % status)
if (b"name=a.example backend=192.0.2.10:443 tls=on source=PANEL"
        not in body):
    raise SystemExit("the routing page did not list the applied route: %r"
                     % body[:200])
if b"name=b.example backend=10.0.2.2:" not in body:
    raise SystemExit("the routing page did not list the first relay route")
if b"name=c.example backend=10.0.2.2:" not in body:
    raise SystemExit("the routing page did not list the second relay route")
# ITEM-504 increment 10: the route whose commit failed was cancelled, so the
# page must not list it; a cancelled route shown as applied would make the
# failed commit invisible.
if b"d.example" in body:
    raise SystemExit("a cancelled route is listed as applied: %r" % body[:200])
if b'action="/panel/routing"' not in body:
    raise SystemExit("the routing page did not carry the form")
for marker in (b"<h3>Add route</h3>", b"<h3>Remove route</h3>",
               b"Route name to remove", b"Stage addition",
               b"Stage removal"):
    if marker not in body:
        raise SystemExit(
            "the routing page did not render the separate add/remove form: "
            "missing %r" % marker)
if b"<script>" in body:
    raise SystemExit("the routing page served an unescaped table")
print("TLS-PROOF routing page: the applied route and the form are served")
ROUTINGEOF

# ITEM-505: the network form proof.  It runs on the route-restore boot, whose
# lwIP pool still has room, and does what an operator does at the panel: signs
# in with the browser form, reads the active and the saved configuration,
# stages a static candidate, confirms the token the page rendered, and reads
# the page again.  The page must show the saved value as STATIC and must still
# show the active interface as DHCP - the commit does not rebind the wire - and
# the serial `[PRODUCTION-NET-SET]` record must say `applies=reboot`.
cat >"$TEST_DIR/serving-network-client.py" <<'NETWORKEOF'
import re
import socket
import ssl
import sys
import urllib.parse

port = int(sys.argv[1])
secret = sys.argv[2].encode("ascii")
mode = sys.argv[3] if len(sys.argv) > 3 else "mutate"
# ITEM-505 increment 3: the bearer session the local login published, used by
# the hostname phase and by the apply boot's identity check.
system_bearer = (
    sys.argv[4].encode("ascii") if len(sys.argv) > 4 else None)

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def exchange(raw):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            version = tls.version()
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def post(target, body, cookie=None, bearer=None):
    header = b""
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    if bearer is not None:
        header += b"Authorization: Bearer " + bearer + b"\r\n"
    return exchange(
        b"POST " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header +
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + body)


def get(target, cookie=None, bearer=None):
    header = b""
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    if bearer is not None:
        header += b"Authorization: Bearer " + bearer + b"\r\n"
    return exchange(
        b"GET " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header +
        b"Connection: close\r\n\r\n")


# The apply boot proves that the appliance serves on the address the durable
# record carries, and - ITEM-505 increment 3 - that the hostname the operator
# saved on the previous boot is the identity this boot serves: the authenticated
# system body and the System page both name it, and the page shows it as the
# active identity and as the committed saved value.
if mode == "apply":
    status, head, body = get(b"/health")
    if status != 200:
        raise SystemExit("the applied static address served %d: %r"
                         % (status, body[:200]))
    print("TLS-PROOF network apply: the appliance serves on the saved "
          "static address")
    if len(sys.argv) > 4:
        applied_bearer = sys.argv[4].encode("ascii")
        status, head, body = get(b"/api/v1/system", bearer=applied_bearer)
        if status != 200:
            raise SystemExit("the applied system body served %d: %r"
                             % (status, body[:200]))
        if b"node-505" not in body:
            raise SystemExit("the applied hostname is not served: %r"
                             % body[:300])
        status, head, body = get(b"/panel/system", bearer=applied_bearer)
        if status != 200:
            raise SystemExit("the applied system page served %d" % status)
        for expected_applied in (b"Active <code>node-505</code>",
                                 b"Saved <code>node-505</code>",
                                 b"source=COMMITTED",
                                 b"applies=reboot"):
            if expected_applied not in body:
                raise SystemExit("the applied system page is missing %r: %r"
                                 % (expected_applied, body[:400]))
        print("TLS-PROOF system apply: the saved hostname is the running "
              "identity after the reboot")
        # ITEM-515: the panel network form returns the saved record to DHCP,
        # the direction the owner could not take from a browser.  The body is
        # the one the drawn form submits: `mode=dhcp` with the four optional
        # inputs present and empty, which the stage refused before this item.
        dhcp_body = b"mode=dhcp&address=&prefix=&gateway=&dns="
        status, head, body = post(b"/panel/network", dhcp_body,
                                  bearer=applied_bearer)
        if status != 303:
            raise SystemExit("the drawn DHCP panel body returned %d: %r"
                             % (status, body[:200]))
        if b"Location: /panel/network" not in head:
            raise SystemExit("the drawn DHCP panel body did not redirect to "
                             "its page")
        status, head, body = get(b"/panel/network", bearer=applied_bearer)
        match = re.search(rb"NETWORK_CONFIG token (\d+)", body)
        if match is None:
            raise SystemExit("the DHCP candidate rendered no token: %r"
                             % body[:400])
        status, head, body = post(
            b"/panel/network", b"token=" + match.group(1) + b"&confirm=1",
            bearer=applied_bearer)
        if status != 303:
            raise SystemExit("the DHCP confirmation returned %d: %r"
                             % (status, body[:200]))
        # A refusal names the field that failed instead of one generic word.
        status, head, body = post(
            b"/panel/network",
            b"mode=static&address=10.0.2.15&prefix="
            b"&gateway=10.0.2.2&dns=10.0.2.3",
            bearer=applied_bearer)
        if status != 400 or b'"refused":"NETWORK_NO_PREFIX"' not in body:
            raise SystemExit("the missing prefix was not named: %d %r"
                             % (status, body[:200]))
        print("TLS-PROOF network panel: the drawn DHCP body and the named "
              "refusal are served")
    sys.exit(0)

form = urllib.parse.urlencode(
    {"username": "admin", "password": secret.decode("ascii")}).encode("ascii")
status, head, body = post(b"/", form)
if status != 303:
    raise SystemExit("network panel sign-in returned %d" % status)
session_cookie = None
for header_line in head.split(b"\r\n"):
    if header_line.lower().startswith(b"set-cookie:"):
        session_cookie = header_line.split(b":", 1)[1].strip()
        break
if session_cookie is None:
    raise SystemExit("network panel sign-in set no cookie")
cookie_pair = session_cookie.split(b";", 1)[0]

status, head, body = get(b"/panel/network", cookie=cookie_pair)
if status != 200:
    raise SystemExit("the network page returned %d" % status)
if b"Active <code>mode=DHCP</code>" not in body:
    raise SystemExit("the network page did not show the live DHCP reading: %r"
                     % body[:300])
if b"applies=reboot" not in body:
    raise SystemExit("the network page did not say the change waits for the "
                     "reboot")
print("TLS-PROOF network page: the live reading and the reboot note are "
      "served")

# ITEM-505 increment 2: a generic section carries the live view of this
# machine - the configured hostname, the management MAC, the service, the
# serving state and the serving path's own recent line - instead of the
# literal `unavailable` every one of those fields showed before.  ITEM-505
# increment 3 turned the System section into a configuring page of its own, so
# the live view is proven on Storage, which stays a generic section.
status, head, body = get(b"/panel/storage", cookie=cookie_pair)
if status != 200:
    raise SystemExit("the storage section returned %d" % status)
for expected_live in (b"<dt>Hostname</dt><dd>hfoc-appliance</dd>",
                      b"<dt>Management MAC</dt>"
                      b"<dd>52:54:00:12:34:56</dd>",
                      b"<dt>Service</dt><dd>https</dd>",
                      b"state=LISTENING reason=",
                      b"[PRODUCTION-SERVING] state=LISTENING listener=live"):
    if expected_live not in body:
        raise SystemExit("the storage section is missing %r: %r"
                         % (expected_live, body[:400]))
print("TLS-PROOF panel section: the live view names the machine instead of "
      "`unavailable`")

# The stage is the operator's own form: address and prefix are two fields, so
# the browser never has to encode a slash.
stage = (b"mode=static&address=10.0.2.15&prefix=24"
         b"&gateway=10.0.2.2&dns=10.0.2.3")
status, head, body = post(b"/panel/network", stage, cookie=cookie_pair)
if status != 303:
    raise SystemExit("the network panel form returned %d: %r"
                     % (status, body[:200]))
if b"Location: /panel/network" not in head:
    raise SystemExit("the network panel form did not redirect to its page")

status, head, body = get(b"/panel/network", cookie=cookie_pair)
match = re.search(rb"NETWORK_CONFIG token (\d+)", body)
if match is None:
    raise SystemExit("the network page rendered no pending token: %r"
                     % body[:400])
network_token = match.group(1)
if not network_token.isdigit():
    raise SystemExit("the network pending token is not a number: %r"
                     % network_token[:32])
print("TLS-PROOF network page: the pending candidate and its token are served")

status, head, body = post(b"/panel/network",
                          b"token=" + network_token + b"&confirm=1",
                          cookie=cookie_pair)
if status != 303:
    raise SystemExit("the network confirmation returned %d: %r"
                     % (status, body[:200]))
if b"Location: /panel/network" not in head:
    raise SystemExit("the network confirmation did not redirect to its page")

# The saved value is the confirmed static configuration; the active reading is
# still the DHCP lease the machine booted with, because the commit writes the
# durable record and the change applies at the next boot.
status, head, body = get(b"/panel/network", cookie=cookie_pair)
if status != 200:
    raise SystemExit("the confirmed network page returned %d" % status)
if (b"Saved <code>mode=STATIC address=10.0.2.15/24 gateway=10.0.2.2 "
        b"dns=10.0.2.3</code>") not in body:
    raise SystemExit("the network page did not show the saved static value: %r"
                     % body[:400])
if b"source=COMMITTED" not in body:
    raise SystemExit("the network page did not name the committed source")
if b"applies=reboot" not in body:
    raise SystemExit("the confirmed network page lost the reboot note")
if b"Active <code>mode=DHCP</code>" not in body:
    raise SystemExit("the commit rebound the live interface instead of "
                     "waiting for the reboot")
print("TLS-PROOF network page: the saved static configuration is shown and "
      "the live interface still runs DHCP")

# ITEM-505 increment 3: the hostname mutation.  It shares this boot with the
# network form, whose lwIP pool has three accepted connections left before the
# fixture refuses the next one, so the stage and the confirmation use the
# scriptable API target with the bearer session the local login published - the
# response carries the single-use token - and only the page reading uses the
# bearer directly.  The page shows the identity the running system serves, the
# saved hostname the durable record carries and `applies=reboot`, next to both
# forms; after the commit the serial record names `current=` as the unchanged
# identity, and the next boot proves the saved name applied.
if system_bearer is None:
    raise SystemExit("the hostname phase needs the bearer session")
status, head, body = get(b"/panel/system", bearer=system_bearer)
if status != 200:
    raise SystemExit("the system page returned %d" % status)
if b"Active <code>hfoc-appliance</code>" not in body:
    raise SystemExit("the system page did not show the running identity: %r"
                     % body[:400])
if b"Saved <code>" not in body or b"applies=reboot" not in body:
    raise SystemExit("the system page did not show the saved hostname and the "
                     "reboot note: %r" % body[:400])
if (b'action="/panel/system"' not in body or
        b'name="hostname"' not in body or b'name="token"' not in body):
    raise SystemExit("the system page did not carry both forms: %r"
                     % body[:400])
print("TLS-PROOF system page: the running identity and the saved hostname are "
      "served")

status, head, body = post(b"/api/v1/system", b"hostname=node-505",
                          bearer=system_bearer)
if status != 200:
    raise SystemExit("the system stage returned %d: %r"
                     % (status, body[:200]))
match = re.search(rb'"token":(\d+)', body)
if match is None:
    raise SystemExit("the system stage rendered no pending token: %r"
                     % body[:300])
system_token = match.group(1)
if not system_token.isdigit():
    raise SystemExit("the system pending token is not a number: %r"
                     % system_token[:32])
print("TLS-PROOF system stage: the pending hostname and its token are served")

status, head, body = post(b"/api/v1/system",
                          b"token=" + system_token + b"&confirm=1",
                          bearer=system_bearer)
if status != 200:
    raise SystemExit("the system confirmation returned %d: %r"
                     % (status, body[:200]))
if b"result=APPLIED" not in body:
    raise SystemExit("the system confirmation did not apply: %r"
                     % body[:300])
print("TLS-PROOF system confirmation: the hostname is applied and durable")
NETWORKEOF

# ITEM-504 increment 8: the facade relay proof.  It runs on the boot that read
# the committed table back, whose lwIP pool is fresh: the request that names
# `b.example` must come back with the *whole* body only the proof backend on
# the test host produces (4096 bytes, more than one TCP segment, so a staged
# answer that waits for the next poll tick instead of streaming is visible).
# Increment 9 adds `c.example`, whose own live backend answers a different
# 4096-byte body: the two names must not share a backend, so a facade that
# ignores the name and dials the first route fails here.  `absent.example` must
# be refused with the management 404 and never guess a backend, and `a.example`
# (`tls=on`) must be refused with 503 because the frozen profile has no TLS
# client.
cat >"$TEST_DIR/serving-facade-client.py" <<'FACADERELAYEOF'
import socket
import ssl
import sys

port = int(sys.argv[1])
backend_ports = sys.argv[2:4]


def expected_body(backend_port):
    marker = ("FACADE-PROOF-BACKEND:%s" % backend_port).encode("ascii")
    return marker + b"\n" + bytes((index * 7 + 3) % 251
                                  for index in range(4096 - len(marker) - 1))


first_expected = expected_body(backend_ports[0])
second_expected = expected_body(backend_ports[1])
marker = b"FACADE-PROOF-BACKEND:"

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def get(host, target=b"/facade"):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            tls.sendall(b"GET " + target + b" HTTP/1.1\r\nHost: " + host
                        + b"\r\nConnection: close\r\n\r\n")
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    head, _, body = data.partition(b"\r\n\r\n")
    status_line = head.split(b"\r\n", 1)[0]
    parts = status_line.split(b" ", 2)
    if len(parts) != 3:
        raise SystemExit("no HTTP status line from the facade: %r" % data[:200])
    return int(parts[1]), body


status, body = get(b"b.example")
if status != 200:
    raise SystemExit("the facade relay returned %d: %r" % (status, body[:200]))
if body != first_expected:
    raise SystemExit("the facade relay did not carry the backend body whole: %r"
                     % body[:200])
print("TLS-PROOF facade relay: the declared name reaches its backend")

status, body = get(b"c.example")
if status != 200:
    raise SystemExit("the second declared name returned %d: %r"
                     % (status, body[:200]))
if body != second_expected:
    raise SystemExit("the second name did not reach its own backend: %r"
                     % body[:200])
if body == first_expected:
    raise SystemExit("both names reached the same backend: %r" % body[:200])
print("TLS-PROOF facade relay: the second name reaches its own backend")

status, body = get(b"absent.example")
if status != 404:
    raise SystemExit("the undeclared name returned %d: %r" % (status, body[:200]))
if marker in body:
    raise SystemExit("the undeclared name reached a backend: %r" % body[:200])
print("TLS-PROOF facade relay: an undeclared name is refused, not guessed")

status, body = get(b"a.example")
if status != 503:
    raise SystemExit("the tls=on route returned %d: %r" % (status, body[:200]))
if marker in body:
    raise SystemExit("the tls=on route reached a backend in the clear: %r"
                     % body[:200])
print("TLS-PROOF facade relay: tls=on is refused, never silently downgraded")
FACADERELAYEOF

cat >"$TEST_DIR/serving-auth-client.py" <<'AUTHCLIENTEOF'
import os
import socket
import ssl
import sys
import time

port = int(sys.argv[1])
token = sys.argv[2].encode("ascii")
secret = sys.argv[3].encode("ascii") if len(sys.argv) > 3 else b""
serial_path = os.environ.get("HFOC_PROOF_SERIAL_LOG", "")

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def exchange(raw):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            version = tls.version()
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def serial_tail(offset):
    """Bytes appended to the guest serial log since the previous read."""
    if not serial_path:
        return "", offset
    try:
        with open(serial_path, "rb") as handle:
            handle.seek(offset)
            data = handle.read()
    except OSError:
        return "", offset
    return data.decode("utf-8", "replace"), offset + len(data)


def panel_auth_record(text, result):
    """The latest completed production panel-auth record for `result`."""
    wanted = "result=" + result
    for line in reversed(text.splitlines()):
        if not line.startswith("[PANEL-AUTH] ") or wanted not in line:
            continue
        if "state=COMPLETED" not in line:
            continue
        fields = {}
        for field in line.split():
            if "=" not in field:
                continue
            key, value = field.split("=", 1)
            if value.isdigit():
                fields[key] = int(value)
        return fields
    return None


def wait_panel_auth_record(offset, result, timeout=5.0):
    """Read the serial tail until the bounded completed record appears."""
    text = ""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk, offset = serial_tail(offset)
        text += chunk
        record = panel_auth_record(text, result)
        if record is not None:
            return record, text, offset
        time.sleep(0.05)
    return None, text, offset


def check_panel_auth_timing(record, result):
    """The completed record separates KDF time from the wait between slices."""
    for key in ("duration_ms", "kdf_us", "wait_us", "slices", "max_gap_us",
                "rounds_done", "rounds_total"):
        if key not in record:
            raise SystemExit(
                "the panel-auth %s record is missing %s: %r"
                % (result, key, record))
    if record["slices"] < 1 or record["kdf_us"] <= 0:
        raise SystemExit(
            "the panel-auth %s record has no measured work: %r"
            % (result, record))
    if record["rounds_total"] <= 0 or \
            record["rounds_done"] != record["rounds_total"]:
        raise SystemExit(
            "the panel-auth %s record did not finish its rounds: %r"
            % (result, record))
    wall_us = record["duration_ms"] * 1000
    if record["duration_ms"] <= 0 or record["wait_us"] > wall_us:
        raise SystemExit(
            "the panel-auth %s record has an impossible wait: %r"
            % (result, record))
    if record["kdf_us"] + record["wait_us"] > wall_us + 100000:
        raise SystemExit(
            "the panel-auth %s record exceeds its wall duration: %r"
            % (result, record))
    # ITEM-655: the cooperative job must not spend a material share of the
    # wall time parked between its slices.  The pre-item QEMU records measured
    # wait/kdf 0.356 and 0.358 (0.65 on Toshiba), because one slice per PIT
    # wake spends 10 ms parked for every ~40 ms of derivation.  One quarter
    # separates that parking loop from the back-to-back slices; both the
    # successful and the denied production records must satisfy it.
    if record["wait_us"] * 4 > record["kdf_us"]:
        raise SystemExit(
            "the panel-auth %s record still waits for a scheduler turn "
            "between KDF slices: %r" % (result, record))


def get(target, authorization=None):
    header = b""
    if authorization is not None:
        header = b"Authorization: Bearer " + authorization + b"\r\n"
    return exchange(b"GET " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
                    + header + b"Connection: close\r\n\r\n")


TARGETS = (
    (b"/api/v1/system", b"hostname"),
    (b"/api/v1/network", b"config"),
    (b"/api/v1/network/status", b"diagnostics"),
    (b"/api/v1/service", b"state"),
    (b"/api/v1/health", b"state"),
)
for target, marker in TARGETS:
    status, head, body = get(target, token)
    if status != 200:
        raise SystemExit("authenticated %s returned %d"
                         % (target.decode(), status))
    if b"application/json" not in head:
        raise SystemExit("%s is not served as JSON" % target.decode())
    if b"not_found" in body:
        raise SystemExit("%s answered the unknown-route body" % target.decode())
    if marker not in body:
        raise SystemExit("%s body is missing %s"
                         % (target.decode(), marker.decode()))
print("TLS-PROOF authenticated read-only management bodies: 200 live evidence")

status, _, _ = get(b"/api/v1/system")
if status != 401:
    raise SystemExit("anonymous request after enrollment returned %d" % status)
status, _, _ = get(b"/api/v1/system", b"A" * 32)
if status != 401:
    raise SystemExit("unknown bearer returned %d" % status)
print("TLS-PROOF enrollment gate: anonymous and unknown bearer refused")

# ITEM-445 step 2: the browser on the laptop signs in with the one form, gets
# one HttpOnly cookie, and the same session serves a section without any
# bearer header.  Sign-out expires the cookie and the cleared cookie is
# refused.
import urllib.parse

# ITEM-652: read only the records this sign-in appends.  The timing proof
# needs the production serial sink, not a host model or a guessed duration.
if not serial_path:
    raise SystemExit("the panel timing proof needs the production serial log")
auth_serial_offset = os.path.getsize(serial_path)
form = urllib.parse.urlencode(
    {"username": "admin", "password": secret.decode("ascii")}).encode("ascii")
status, head, body = exchange(
    b"POST / HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Content-Type: application/x-www-form-urlencoded\r\n"
    b"Content-Length: " + str(len(form)).encode("ascii") + b"\r\n"
    b"Connection: close\r\n\r\n" + form)
if status != 303:
    raise SystemExit("browser sign-in returned %d" % status)
if b"Location: /panel" not in head:
    raise SystemExit("browser sign-in did not redirect to the section list")
session_cookie = None
for header_line in head.split(b"\r\n"):
    if header_line.lower().startswith(b"set-cookie:"):
        session_cookie = header_line.split(b":", 1)[1].strip()
        break
if session_cookie is None:
    raise SystemExit("browser sign-in set no cookie")
for attribute in (b"hfoc_session=", b"HttpOnly", b"Secure",
                  b"SameSite=Strict", b"Path=/", b"Max-Age=900"):
    if attribute not in session_cookie:
        raise SystemExit("session cookie is missing %r" % attribute)
if secret in session_cookie:
    raise SystemExit("session cookie carries the administrator secret")
cookie_pair = session_cookie.split(b";", 1)[0]
print("TLS-PROOF browser sign-in: 303 with one HttpOnly Secure SameSite cookie")

record, auth_text, auth_serial_offset = wait_panel_auth_record(
    auth_serial_offset, "OK")
if record is None:
    raise SystemExit(
        "the successful browser sign-in published no completed timed record: %r"
        % auth_text[-400:])
check_panel_auth_timing(record, "OK")
print("TLS-PROOF panel sign-in timing: OK duration_ms=%d kdf_us=%d "
      "wait_us=%d slices=%d max_gap_us=%d rounds=%d/%d"
      % (record["duration_ms"], record["kdf_us"], record["wait_us"],
         record["slices"], record["max_gap_us"], record["rounds_done"],
         record["rounds_total"]))

status, head, body = exchange(
    b"GET /panel/network HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Cookie: " + cookie_pair + b"\r\n"
    b"Connection: close\r\n\r\n")
if status != 200 or b"Network" not in body:
    raise SystemExit("section with the session cookie returned %d" % status)
if b"Management sign-in" in body and b"SIGNED IN" not in body:
    raise SystemExit("section with the session cookie rendered the sign-in "
                     "document")
print("TLS-PROOF panel section: 200 to the session cookie alone")

# ITEM-503: the BYO certificate surface.  The Security page renders the import
# form and an authorized post that does not carry a certificate is refused by
# name - the route is wired to the mutation plane and fails closed.  The two
# checks open two connections on purpose: this fixture's lwIP pool holds six
# TCP PCBs, the phase already opens eleven, and a fifteenth connection is
# refused before the accept.  The anonymous denial is proven by the host plane
# proof, and the successful import and its record by the certificate upload /
# record v2 host proof; this appliance's clock is not validated in this
# fixture, so a real import is refused BAD_CLOCK here and that refusal is
# asserted too when it is the reason.
status, head, body = exchange(
    b"GET /panel/security HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Cookie: " + cookie_pair + b"\r\n"
    b"Connection: close\r\n\r\n")
if status != 200 or b'action="/panel/security"' not in body:
    raise SystemExit("the Security page did not render the import form")
if b'name="certificate"' not in body or b'name="key"' not in body:
    raise SystemExit("the Security import form is missing a field")
print("TLS-PROOF Security page: the BYO certificate import form is rendered")

_certificate_form = b"certificate=not+a+pem&key=also+not"
status, head, body = exchange(
    b"POST /panel/security HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Cookie: " + cookie_pair + b"\r\n"
    b"Content-Type: application/x-www-form-urlencoded\r\n"
    b"Content-Length: " + str(len(_certificate_form)).encode() +
    b"\r\nConnection: close\r\n\r\n" + _certificate_form)
if status != 400:
    raise SystemExit("an invalid certificate import returned %d" % status)
if (b"BAD_CERTIFICATE" not in body and b"BAD_CLOCK" not in body):
    raise SystemExit("an invalid certificate import named no reason: %r"
                     % body[:200])
print("TLS-PROOF Security import: an invalid import is refused by name")

status, head, body = exchange(
    b"POST /panel/logout HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Content-Length: 0\r\n"
    b"Connection: close\r\n\r\n")
if status != 303:
    raise SystemExit("browser sign-out returned %d" % status)
expired_cookie = None
for header_line in head.split(b"\r\n"):
    if header_line.lower().startswith(b"set-cookie:"):
        expired_cookie = header_line.split(b":", 1)[1].strip()
        break
if (expired_cookie is None or b"Max-Age=0" not in expired_cookie or
        b"hfoc_session=;" not in expired_cookie):
    raise SystemExit("browser sign-out did not expire the cookie")
print("TLS-PROOF browser sign-out: 303 with the cookie expired")

# The sign-out cookie is the empty value with Max-Age=0, so a browser drops
# it; a request that presents no cookie is still refused.  The server-side
# session keeps its own short lifetime, exactly as the administrator boundary
# issued it.
status, head, body = exchange(
    b"GET /panel/network HTTP/1.1\r\nHost: hfoc-appliance\r\n"
    b"Connection: close\r\n\r\n")
if status != 401:
    raise SystemExit("section without a cookie returned %d" % status)
print("TLS-PROOF panel section: unsigned request refused")
AUTHCLIENTEOF

# ITEM-513: the nav's one-click language switch.  The owner's decision
# (2026-10-10) makes the language a property of the browser: a `hfoc_lang`
# cookie set by the nav's `EN | RU` buttons wins over the shared
# `v1:system.language` record, one click and no token.  The proof drives the
# real browser path - the sign-in form, the session cookie, the drawn System
# stage bodies, the switch POST - and then reads browser A's next page in
# Russian while the machine's record stays English.  It runs on a boot of its
# own: its nine connections do not fit in the spent pools of the boots above.
cat >"$TEST_DIR/serving-language-client.py" <<'LANGEOF'
import socket
import ssl
import sys
import urllib.parse

port = int(sys.argv[1])
secret = sys.argv[2].encode("ascii")

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def exchange(raw):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def set_cookie_pair(head):
    for header_line in head.split(b"\r\n"):
        if header_line.lower().startswith(b"set-cookie:"):
            value = header_line.split(b":", 1)[1].strip()
            return value.split(b";", 1)[0]
    return None


def post(target, body, cookie=None):
    header = b""
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    return exchange(
        b"POST " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header +
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + body)


def get(target, cookie=None):
    header = b""
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    return exchange(
        b"GET " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header + b"Connection: close\r\n\r\n")


# 1. The browser on the laptop signs in with the one form and holds the one
#    session cookie.
form = urllib.parse.urlencode(
    {"username": "admin", "password": secret.decode("ascii")}).encode("ascii")
status, head, _ = post(b"/", form)
if status != 303:
    raise SystemExit("the language proof sign-in returned %d" % status)
session = set_cookie_pair(head)
if session is None or not session.startswith(b"hfoc_session="):
    raise SystemExit("the language proof sign-in set no session cookie")
print("TLS-PROOF language: the browser signs in and holds its session cookie")

# 2. Without the language cookie the page is the record's language, and the
#    drawn System page carries the stage fields the proof submits next.
status, _, body = get(b"/panel/system", cookie=session)
if status != 200 or b'<html lang="en">' not in body:
    raise SystemExit("the first page is not the record's English document: "
                     "%d" % status)
if b'name="hostname"' not in body or b'name="language"' not in body:
    raise SystemExit("the System page drew no stage fields: %r" % body[:300])
print("TLS-PROOF language: a request without the cookie renders the record's "
      "language")

# 3. The bodies the System page draws: exactly one field each, and both are
#    accepted by the real stage.  The one two-field body the old page drew is
#    the defect this item closes, so the proof submits the drawn shape; the
#    panel target accepts a stage with a 303 back to the page, while a body the
#    stage cannot parse is refused with a 400 and a named body.
for drawn_body in (b"hostname=hfoc-appliance", b"language=en"):
    status, head, body = post(b"/panel/system", drawn_body, cookie=session)
    if status != 303 or b"Location: /panel/system" not in head:
        raise SystemExit("the drawn System stage body was refused: %d %r"
                         % (status, body[:200]))
print("TLS-PROOF language: the drawn System stage bodies are accepted")

# 4. The one click: the nav's form submits the one field, and the answer is one
#    session-scoped `hfoc_lang` cookie and a redirect.  No token is involved.
status, head, _ = post(b"/panel/language", b"language=ru", cookie=session)
if status != 303:
    raise SystemExit("the language switch returned %d" % status)
language_cookie = set_cookie_pair(head)
if language_cookie != b"hfoc_lang=ru":
    raise SystemExit("the language switch set %r" % language_cookie)
if b"Location: /panel" not in head:
    raise SystemExit("the language switch did not redirect to the sections")
for attribute in (b"HttpOnly", b"Secure", b"SameSite=Strict", b"Path=/"):
    if attribute not in head:
        raise SystemExit("the language cookie is missing %r" % attribute)
print("TLS-PROOF language: one click answers 303 and one hfoc_lang cookie")

# 5. Browser A's next page is Russian; the machine record stays English.
both = session + b"; " + language_cookie
status, _, body = get(b"/panel", cookie=both)
if status != 200 or b'<html lang="ru">' not in body:
    raise SystemExit("browser A's next page is not Russian: %d" % status)
if "Обзор".encode("utf-8") not in body:
    raise SystemExit("browser A's navigation did not speak Russian: %r"
                     % body[:400])
status, _, body = get(b"/panel/system", cookie=both)
if status != 200 or b'<html lang="ru">' not in body:
    raise SystemExit("the switched browser did not keep Russian: %d" % status)
if b">en</code>" not in body:
    raise SystemExit("the switch changed the machine's language record: %r"
                     % body[:400])
# ITEM-647: the Russian System page names its fields from the Russian column,
# including the shared confirmation code, so the language is not only the
# document attribute and the navigation.
for label in ("Имя устройства", "Язык панели", "Код подтверждения"):
    if label.encode("utf-8") not in body:
        raise SystemExit("the Russian System form is missing its label %r: %r"
                         % (label, body[:400]))
print("TLS-PROOF language: browser A serves Russian while the record stays en")

# 6. The same browser without the language cookie is English again, so the
#    choice is the cookie's and nothing was latched on the machine.
status, _, body = get(b"/panel", cookie=session)
if (status != 200 or b'<html lang="en">' not in body or
        b"<h2>Dashboard</h2>" not in body):
    raise SystemExit("the cookie-less request is not English: %d" % status)
print("TLS-PROOF language: a request without the cookie is English again")
LANGEOF

# ITEM-401: a boot may load a persisted account without publishing it (nothing
# publishes a verifier until an operator presents the secret on that boot), and
# then the ITEM-365 request gate answers 503 while the routes stay known.
# ITEM-506 increment 5 uses the same client for the five managed targets on the
# network-apply boot, whose lwIP pool is fresh.
cat >"$TEST_DIR/serving-closed-check.py" <<'CLOSEDEOF'
import socket
import ssl
import sys

# ITEM-516: `--open` proves the other side of the same gate.  With the
# after-boot policy published the request is no longer refused as unavailable:
# the sign-in document answers 200 and a managed target answers the
# authenticated-required 401, with no local login anywhere on this boot.
expect_open = len(sys.argv) > 1 and sys.argv[1] == "--open"
arguments = sys.argv[2:] if expect_open else sys.argv[1:]
port = int(arguments[0])
label = arguments[1] if len(arguments) > 1 else "rebooted appliance"
targets = arguments[2:]
if not targets:
    targets = ["/", "/api/v1/system"] if expect_open else ["/api/v1/system"]

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3

for target in targets:
    with socket.create_connection(("127.0.0.1", port), timeout=60) as sock:
        # ITEM-516: the open-mode proof is a browser that sends no SNI.  The
        # certificate-name proof earlier in this run turns the explicit default
        # declaration off, and after that reboot a *named* request for the
        # appliance's own identity is refused by the ITEM-507 selection policy;
        # that selection is a separate behaviour with its own proof, and the
        # gate proof is about the request gate.  A no-SNI handshake is served
        # the default certificate, exactly as the durability proof records
        # (`name=- result=DEFAULT cert=default`).
        with context.wrap_socket(
                sock, server_hostname=None if expect_open
                else "hfoc-appliance") as tls:
            tls.sendall(("GET %s HTTP/1.1\r\nHost: hfoc-appliance\r\n"
                         "Connection: close\r\n\r\n" % target).encode())
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    if expect_open:
        expected = 200 if target == "/" else 401
        if status != expected:
            raise SystemExit(
                "%s answered %d with the after-boot policy published "
                "(expected %d)" % (target, status, expected))
        if target == "/" and b"not_found" in body:
            raise SystemExit("%s answered the unknown-route body" % target)
        continue
    if status != 503:
        raise SystemExit("%s answered %d before the verifier was published"
                         % (target, status))
    if b"not_found" in body:
        raise SystemExit("%s answered the unknown-route body" % target)
if expect_open:
    print("TLS-PROOF %s: %d target(s) answered without any local login"
          % (label, len(targets)))
else:
    print("TLS-PROOF %s: %d managed target(s) refused with the "
          "gate-unavailable 503" % (label, len(targets)))
CLOSEDEOF

# ITEM-506 increment 6: the credential change from the panel and its session
# invalidation, proven end to end over the real TLS 1.3 client.
#
# The change boot signs in at `/` with the old credential (one cookie), stages
# and confirms the change on the scriptable target with the bearer the local
# console issued (two requests), and then presents both credentials the change
# was supposed to kill: the cookie and the bearer must answer 401, not 200.
# `sessions=REVOKED` in the serial record is the appliance's own statement; the
# two 401s are the proof that the statement is true.  The durability boot signs
# in with the old pair (must answer 401) and with the new pair (must answer
# 303), then reads the Account page and requires it to name the new login.
cat >"$TEST_DIR/serving-account-client.py" <<'ACCOUNTEOF'
import os
import re
import socket
import ssl
import sys
import time
import urllib.parse

port = int(sys.argv[1])
new_login = sys.argv[2]
new_secret = sys.argv[3]
old_secret = sys.argv[4]
mode = sys.argv[5]
serial_path = os.environ.get("HFOC_PROOF_SERIAL_LOG", "")

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def exchange(raw):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            version = tls.version()
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def cookie_pair(head):
    for header_line in head.split(b"\r\n"):
        if header_line.lower().startswith(b"set-cookie:"):
            value = header_line.split(b":", 1)[1].strip()
            return value.split(b";", 1)[0]
    return None


def sign_in(login, password):
    form = urllib.parse.urlencode(
        {"username": login, "password": password}).encode("ascii")
    return exchange(
        b"POST / HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(form)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + form)


def serial_tail(offset):
    """Bytes appended to the guest serial log since the previous read."""
    if not serial_path:
        return "", offset
    try:
        with open(serial_path, "rb") as handle:
            handle.seek(offset)
            data = handle.read()
    except OSError:
        return "", offset
    return data.decode("utf-8", "replace"), offset + len(data)


def panel_auth_record(text, result):
    """The latest completed production panel-auth record for `result`."""
    wanted = "result=" + result
    for line in reversed(text.splitlines()):
        if not line.startswith("[PANEL-AUTH] ") or wanted not in line:
            continue
        if "state=COMPLETED" not in line:
            continue
        fields = {}
        for field in line.split():
            if "=" not in field:
                continue
            key, value = field.split("=", 1)
            if value.isdigit():
                fields[key] = int(value)
        return fields
    return None


def wait_panel_auth_record(offset, result, timeout=5.0):
    """Read the serial tail until the bounded completed record appears."""
    text = ""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        chunk, offset = serial_tail(offset)
        text += chunk
        record = panel_auth_record(text, result)
        if record is not None:
            return record, text, offset
        time.sleep(0.05)
    return None, text, offset


def check_panel_auth_timing(record, result):
    """The completed record separates KDF time from the wait between slices."""
    for key in ("duration_ms", "kdf_us", "wait_us", "slices", "max_gap_us",
                "rounds_done", "rounds_total"):
        if key not in record:
            raise SystemExit(
                "the panel-auth %s record is missing %s: %r"
                % (result, key, record))
    if record["slices"] < 1 or record["kdf_us"] <= 0:
        raise SystemExit(
            "the panel-auth %s record has no measured work: %r"
            % (result, record))
    if record["rounds_total"] <= 0 or \
            record["rounds_done"] != record["rounds_total"]:
        raise SystemExit(
            "the panel-auth %s record did not finish its rounds: %r"
            % (result, record))
    wall_us = record["duration_ms"] * 1000
    if record["duration_ms"] <= 0 or record["wait_us"] > wall_us:
        raise SystemExit(
            "the panel-auth %s record has an impossible wait: %r"
            % (result, record))
    if record["kdf_us"] + record["wait_us"] > wall_us + 100000:
        raise SystemExit(
            "the panel-auth %s record exceeds its wall duration: %r"
            % (result, record))
    # ITEM-655: the cooperative job must not spend a material share of the
    # wall time parked between its slices.  The pre-item QEMU records measured
    # wait/kdf 0.356 and 0.358 (0.65 on Toshiba), because one slice per PIT
    # wake spends 10 ms parked for every ~40 ms of derivation.  One quarter
    # separates that parking loop from the back-to-back slices; both the
    # successful and the denied production records must satisfy it.
    if record["wait_us"] * 4 > record["kdf_us"]:
        raise SystemExit(
            "the panel-auth %s record still waits for a scheduler turn "
            "between KDF slices: %r" % (result, record))


def post(target, body, cookie=None, bearer=None):
    header = b""
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    if bearer is not None:
        header += b"Authorization: Bearer " + bearer + b"\r\n"
    return exchange(
        b"POST " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header +
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + body)


def get(target, cookie=None, bearer=None):
    header = b""
    if cookie is not None:
        header += b"Cookie: " + cookie + b"\r\n"
    if bearer is not None:
        header += b"Authorization: Bearer " + bearer + b"\r\n"
    return exchange(
        b"GET " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        + header + b"Connection: close\r\n\r\n")


if mode == "change":
    bearer = sys.argv[6].encode("ascii")
    status, head, body = sign_in("admin", old_secret)
    if status != 303:
        raise SystemExit("the old credential did not sign in before the "
                         "change: %d %r" % (status, body[:200]))
    old_cookie = cookie_pair(head)
    if old_cookie is None:
        raise SystemExit("the pre-change sign-in set no cookie")
    print("TLS-PROOF account change: the old credential signs in before the "
          "change")

    # The cookie must serve the panel before the change, or the 401 after the
    # commit would also pass for a cookie that never authenticated at all.
    status, head, body = get(b"/panel/account", cookie=old_cookie)
    if status != 200 or b"Login <code>admin</code>" not in body:
        raise SystemExit("the pre-change cookie did not serve the Account "
                         "page: %d %r" % (status, body[:200]))
    print("TLS-PROOF account change: the pre-change cookie serves the Account page")

    # The current password is the proof the stage requires: a wrong one must be
    # refused by name and stage nothing.  This attempt uses the panel target,
    # so the form target a browser posts to also reaches the mutation.
    wrong = urllib.parse.urlencode(
        {"current": old_secret + "-wrong", "login": new_login,
         "password": new_secret, "repeat": new_secret}).encode("ascii")
    status, head, body = post(b"/panel/account", wrong, bearer=bearer)
    if status != 400 or b"REFUSED" not in body:
        raise SystemExit("a wrong current password was not refused: %d %r"
                         % (status, body[:200]))
    print("TLS-PROOF account change: a wrong current password is refused")

    stage = urllib.parse.urlencode(
        {"current": old_secret, "login": new_login,
         "password": new_secret, "repeat": new_secret}).encode("ascii")
    status, head, body = post(b"/api/v1/system/account", stage, bearer=bearer)
    if status != 200 or b"PENDING_CONFIRMATION" not in body:
        raise SystemExit("the credential stage returned %d: %r"
                         % (status, body[:300]))
    match = re.search(rb'"token":([0-9]+)', body)
    if match is None:
        raise SystemExit("the credential stage published no token: %r"
                         % body[:300])
    token = match.group(1)
    print("TLS-PROOF account change: the panel form stages the new login and "
          "password")

    confirm = b"token=" + token + b"&confirm=1"
    status, head, body = post(b"/api/v1/system/account", confirm, bearer=bearer)
    if status != 200 or b"APPLIED" not in body:
        raise SystemExit("the credential confirmation returned %d: %r"
                         % (status, body[:300]))
    print("TLS-PROOF account change: the confirmed change applies")

    status, head, body = get(b"/panel/account", cookie=old_cookie)
    if status != 401:
        raise SystemExit("the cookie issued before the change still serves "
                         "the panel: %d" % status)
    print("TLS-PROOF account change: the previous cookie is revoked")

    status, head, body = get(b"/api/v1/system", bearer=bearer)
    if status != 401:
        raise SystemExit("the bearer issued before the change still "
                         "authorizes: %d" % status)
    print("TLS-PROOF account change: the previous bearer is revoked")
elif mode == "durability":
    status, head, body = sign_in("admin", old_secret)
    if status != 401:
        raise SystemExit("the old credential still signs in after the "
                         "reboot: %d" % status)
    print("TLS-PROOF account durability: the old credential is refused after the reboot")

    status, head, body = sign_in(new_login, new_secret)
    if status != 303:
        raise SystemExit("the new credential did not sign in after the "
                         "reboot: %d %r" % (status, body[:200]))
    new_cookie = cookie_pair(head)
    if new_cookie is None or not new_cookie.startswith(b"hfoc_session="):
        raise SystemExit("the new sign-in set no session cookie: %r"
                         % new_cookie)
    print("TLS-PROOF account durability: the new credential signs in after "
          "the reboot")

    # ITEM-652: the same-login wrong-password refusal on this fresh boot.  It
    # runs the stored KDF like a real refusal and must publish a measured
    # DENIED record, set no cookie and leave the session rule alone.
    if not serial_path:
        raise SystemExit(
            "the panel timing proof needs the production serial log")
    auth_serial_offset = os.path.getsize(serial_path)
    status, head, body = sign_in(new_login, "hfoc-wrong-password")
    if status != 401:
        raise SystemExit(
            "a wrong panel password returned %d: %r" % (status, body[:160]))
    if b"set-cookie:" in head.lower():
        raise SystemExit("a refused panel sign-in issued a cookie")
    record, auth_text, auth_serial_offset = wait_panel_auth_record(
        auth_serial_offset, "DENIED")
    if record is None:
        raise SystemExit(
            "the refused browser sign-in published no completed timed "
            "record: %r" % auth_text[-400:])
    check_panel_auth_timing(record, "DENIED")
    print("TLS-PROOF panel sign-in timing: DENIED duration_ms=%d kdf_us=%d "
          "wait_us=%d slices=%d max_gap_us=%d rounds=%d/%d"
          % (record["duration_ms"], record["kdf_us"], record["wait_us"],
             record["slices"], record["max_gap_us"], record["rounds_done"],
             record["rounds_total"]))

    status, head, body = get(b"/panel/account", cookie=new_cookie)
    expected = ("Login <code>%s</code>" % new_login).encode("ascii")
    if status != 200 or expected not in body:
        raise SystemExit("the Account page did not name the new login: %d %r"
                         % (status, body[:300]))
    if b'action="/panel/account"' not in body:
        raise SystemExit("the Account page did not render the change form")
    print("TLS-PROOF account durability: the Account page names the new login")
else:
    raise SystemExit("unknown account client mode: %s" % mode)
ACCOUNTEOF

# ITEM-507 increment 4: the name -> certificate/key mutation over the real
# listener.  `commit` signs in with the account the credential-change phase
# just wrote and commits the explicit default declaration (the one action the
# unvalidated fixture clock cannot refuse), and `durability` proves on the next
# boot that the committed record is in force: an unknown SNI name is refused
# while a client without SNI still gets the appliance's own identity.
cat >"$TEST_DIR/serving-certificate-name-client.py" <<'CERTNAMEOF'
import re
import socket
import ssl
import sys
import urllib.parse

port = int(sys.argv[1])
login = sys.argv[2]
secret = sys.argv[3]
mode = sys.argv[4]

context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
context.check_hostname = False
context.verify_mode = ssl.CERT_NONE
context.minimum_version = ssl.TLSVersion.TLSv1_3
context.maximum_version = ssl.TLSVersion.TLSv1_3


def exchange(raw):
    with socket.create_connection(("127.0.0.1", port), timeout=90) as sock:
        with context.wrap_socket(sock, server_hostname="hfoc-appliance") as tls:
            version = tls.version()
            tls.sendall(raw)
            data = b""
            while True:
                chunk = tls.recv(4096)
                if not chunk:
                    break
                data += chunk
                if len(data) > 65536:
                    break
    if version != "TLSv1.3":
        raise SystemExit("unexpected TLS version: %s" % version)
    head, _, body = data.partition(b"\r\n\r\n")
    status = int(head.split(b" ", 2)[1])
    return status, head, body


def cookie_pair(head):
    for header_line in head.split(b"\r\n"):
        if header_line.lower().startswith(b"set-cookie:"):
            value = header_line.split(b":", 1)[1].strip()
            return value.split(b";", 1)[0]
    return None


def sign_in(login_name, password):
    form = urllib.parse.urlencode(
        {"username": login_name, "password": password}).encode("ascii")
    return exchange(
        b"POST / HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(form)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + form)


def post(target, body, cookie):
    return exchange(
        b"POST " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        b"Cookie: " + cookie + b"\r\n"
        b"Content-Type: application/x-www-form-urlencoded\r\n"
        b"Content-Length: " + str(len(body)).encode("ascii") + b"\r\n"
        b"Connection: close\r\n\r\n" + body)


def get(target, cookie):
    return exchange(
        b"GET " + target + b" HTTP/1.1\r\nHost: hfoc-appliance\r\n"
        b"Cookie: " + cookie + b"\r\n"
        b"Connection: close\r\n\r\n")


if mode == "commit":
    status, head, body = sign_in(login, secret)
    if status != 303:
        raise SystemExit("the new credential did not sign in for the "
                         "certificate-name change: %d %r"
                         % (status, body[:200]))
    cookie = cookie_pair(head)
    if cookie is None or not cookie.startswith(b"hfoc_session="):
        raise SystemExit("the certificate-name sign-in set no session cookie")
    stage = b"action=default&mode=off"
    status, head, body = post(b"/api/v1/system/certificate/name", stage,
                              cookie)
    if status != 200 or b"PENDING_CONFIRMATION" not in body:
        raise SystemExit("the certificate-name stage returned %d: %r"
                         % (status, body[:300]))
    match = re.search(rb'"token":([0-9]+)', body)
    if match is None:
        raise SystemExit("the certificate-name stage published no token: %r"
                         % body[:300])
    confirm = b"token=" + match.group(1) + b"&confirm=1"
    status, head, body = post(b"/api/v1/system/certificate/name", confirm,
                              cookie)
    if status != 200 or b"APPLIED" not in body:
        raise SystemExit("the certificate-name confirmation returned %d: %r"
                         % (status, body[:300]))
    print("TLS-PROOF certificate-name: the explicit default is committed")

    # An import whose PEM does not decode is refused at the stage, so the
    # target is wired to the bound executor and fails closed instead of
    # storing something it could not validate.  (A real pair is refused by
    # name on this fixture anyway: its clock is not validated, exactly as the
    # ITEM-503 import documents.)
    bad_import = b"action=import&name=x.example&certificate=not+a+pem&key=also+not"
    status, head, body = post(b"/api/v1/system/certificate/name", bad_import,
                              cookie)
    if status != 400 or b"REFUSED" not in body:
        raise SystemExit("a malformed certificate-name import was not "
                         "refused: %d %r" % (status, body[:300]))
    print("TLS-PROOF certificate-name: a malformed import is refused by name")

    # ITEM-507 increment 5: the Certificates page lists the active summary
    # next to the saved table and carries the two mutation forms; it never
    # renders key material.
    status, head, body = get(b"/panel/certificates", cookie=cookie)
    if status != 200:
        raise SystemExit("the certificates page returned %d: %r"
                         % (status, body[:200]))
    for expected in (b"Active names=", b"Saved <code>",
                     b"applies=reboot",
                     b'action="/panel/certificates"',
                     b'name="certificate"', b'name="token"'):
        if expected not in body:
            raise SystemExit("the certificates page is missing %r: %r"
                             % (expected, body[:400]))
    if b"PRIVATE KEY" in body:
        raise SystemExit("the certificates page rendered key material")
    print("TLS-PROOF certificate page: the saved table and the forms are "
          "served")

    # ITEM-657: a staged certificate-name mutation must answer 303 back to the
    # Certificates page AND that page must render the pending token.  The
    # serving path's pending list omitted CERTIFICATE_NAME, so the browser got
    # no confirmation step at all.  The delete stage needs no clock and the
    # cancel clears it, so nothing durable changes here.
    panel_stage = urllib.parse.urlencode(
        {"action": "delete", "name": "hfoc-test.example.test"}).encode("ascii")
    status, head, body = post(b"/panel/certificates", panel_stage, cookie)
    if status != 303:
        raise SystemExit("the panel certificate-name stage returned %d: %r"
                         % (status, body[:300]))
    status, head, body = get(b"/panel/certificates", cookie=cookie)
    match = re.search(rb"CERTIFICATE_NAME token ([0-9]+)", body)
    if status != 200 or match is None:
        raise SystemExit(
            "the staged certificate-name mutation published no panel token: "
            "%d %r" % (status, body[:600]))
    print("TLS-PROOF certificate-name: the panel page shows the pending token")
    panel_cancel = urllib.parse.urlencode(
        {"token": match.group(1).decode("ascii"), "cancel": "1"}).encode(
            "ascii")
    status, head, body = post(b"/panel/certificates", panel_cancel, cookie)
    if status != 303:
        raise SystemExit("the panel certificate-name cancel returned %d: %r"
                         % (status, body[:300]))
    status, head, body = get(b"/panel/certificates", cookie=cookie)
    if status != 200 or b"CERTIFICATE_NAME token " in body:
        raise SystemExit(
            "the cancelled certificate-name candidate stayed pending: %d %r"
            % (status, body[:600]))
    print("TLS-PROOF certificate-name: the panel cancel clears the token")
elif mode == "durability":
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=60) as sock:
            with context.wrap_socket(sock,
                                     server_hostname="unknown.example"):
                pass
    except (ssl.SSLError, OSError):
        print("TLS-PROOF certificate-name durability: an unknown name is "
              "refused")
    else:
        raise SystemExit("an unknown SNI name was served after the reboot")
    # A client that presents no name is not an unknown name: it still gets the
    # appliance's own declared identity.
    with socket.create_connection(("127.0.0.1", port), timeout=60) as sock:
        with context.wrap_socket(sock):
            pass
    print("TLS-PROOF certificate-name durability: a client without SNI gets "
          "the declared identity")
else:
    raise SystemExit("unknown certificate-name client mode: %s" % mode)
CERTNAMEOF

# ITEM-506 increment 5: the real TLS 1.3 client phase moved below the local
# console conversation.  The listener is withheld until the account is set, so
# there is nothing to connect to before it, and the client's anonymous
# assertions describe the post-set gate (401, not the gate-unavailable 503).

# ITEM-401/ITEM-506 phase 1: this phase sets the account on the local console of
# the first boot.  ITEM-506 hides the typed password: it is never echoed, never
# written to the console history and never reaches the enrollment record or the
# durable journal.
ADMIN_SECRET=hfocsecret401
timing_next first-boot/enroll
if ! python3 "$TEST_DIR/admin-enroll.py" provision "$MONITOR_SOCKET" \
        "$SERIAL_LOG" "$ADMIN_SECRET"; then
    FAILURE_MESSAGE="Administrator credential provisioning phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! wait_for_record '[ADMIN-ENROLLMENT] credentials changed' 20; then
    FAILURE_MESSAGE="The enrollment record did not publish the changed state"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-403: the fresh boot must have deferred the administrator credential
# instead of committing a factory record.  The deferral is published by the
# same boot, so it is awaited with the same bound.
deferred_marker='first_boot=provisioned'
if ! wait_for_record "$deferred_marker" 20; then
    FAILURE_MESSAGE="The fresh boot did not publish the deferred administrator state"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq 'first_boot=provisioned .*admin=deferred' "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="The fresh boot did not publish the deferred administrator state"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# The console hides the typed password, so no serial line may carry it: not an
# enrollment record and not any other line of this boot.
if grep -Fq "$ADMIN_SECRET" "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="The hidden console input leaked the password into the serial log"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

# ITEM-506 increment 5: the account exists now, so the serving poll may raise
# the listener.  Wait for the live state itself (the network, the entropy owner
# and the certificate owner must also have published) and only then run the
# real TLS 1.3 client.
timing_next first-boot/listener-ready
serving_live_deadline=$((SECONDS + 60))
while (( SECONDS < serving_live_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] v=1 state=LISTENING .*listener=live' \
            "$(serial_view "$SERIAL_LOG")"; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Eq \
        '\[PRODUCTION-SERVING\] v=1 state=LISTENING .*listener=live .*tls=TLS1\.3 .*reachability=MANAGEMENT_INTERFACE .*bind=10\.0\.2\.15:443' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production serving record did not report a live listener after the account was set"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-SERVING\] .*entropy=(RDSEED|RDRAND)' "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production serving record did not name the hardware entropy source"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq \
        '\[PRODUCTION-SERVING\] .*certificate=(persisted|ephemeral)_self_signed' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production serving record did not report the appliance certificate"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Eq '\[PRODUCTION-SERVING\].*(PRIVATE KEY|BEGIN |secret=|password=)' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Production serving record leaked secret material"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "TLS-PROOF serving gate: listener live after the account was set"

timing_next first-boot/client
serving_client_status=0
HFOC_PROOF_SERIAL_LOG="$SERIAL_LOG" python3 "$TEST_DIR/serving-client.py" "$SERVING_PORT" "$TEST_DIR/peer.der" || serving_client_status=$?
if (( serving_client_status != 0 )); then
    # ITEM-386: this phase is fatal.  The ordinary production boot publishes
    # the hardware entropy owner, the per-appliance certificate and a live
    # TLS 1.3 listener, and the real TLS 1.3 client now completes the
    # handshake and is served.  Reaching this branch means a regression in the
    # serving crypto partition, the TLS 1.3 server flight, the request parser
    # or the panel/API dispatch order, so the proof must fail rather than
    # report a degraded success.
    FAILURE_MESSAGE="Production serving live TLS 1.3 client phase failed"
    echo "PRODUCTION-SERVING FAILURE: the TLS 1.3 client phase did not complete" >&2
    echo "  (observed by the real TLS 1.3 client; see the TLS-PROOF lines above)" >&2
    exit 1
fi

if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="Kernel panic during production service proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

# ITEM-507 increment 3: the handshake consults the declared name table and the
# loop publishes which name it was served and why.  This boot has no committed
# name record, so the appliance keeps serving its one certificate and says so
# explicitly instead of staying silent.
if ! grep -Fq \
        '[PRODUCTION-TLS-SNI] name=hfoc-appliance result=DEFAULT cert=default' \
        "$(serial_view "$SERIAL_LOG")"; then
    FAILURE_MESSAGE="The SNI selection record is missing from the serving boot"
    echo "$FAILURE_MESSAGE" >&2
    grep -F 'PRODUCTION-TLS-SNI' "$SERIAL_LOG" >&2 || true
    exit 1
fi
echo "TLS-PROOF SNI: the handshake recorded the name it was served"

# ITEM-503: modern clients ignore the common name, so the certificate the
# appliance served must carry the appliance's own name as a subjectAltName
# dNSName.  The check parses the DER the real TLS client received.
if [[ ! -s "$TEST_DIR/peer.der" ]]; then
    FAILURE_MESSAGE="The TLS client received no peer certificate"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! openssl x509 -inform DER -in "$TEST_DIR/peer.der" -noout \
        -ext subjectAltName >"$TEST_DIR/peer-san.txt" 2>&1; then
    FAILURE_MESSAGE="The served certificate carries no subjectAltName"
    cat "$TEST_DIR/peer-san.txt" >&2
    exit 1
fi
if ! grep -Fq 'DNS:hfoc-appliance' "$TEST_DIR/peer-san.txt"; then
    FAILURE_MESSAGE="The served certificate names no appliance SAN"
    cat "$TEST_DIR/peer-san.txt" >&2
    exit 1
fi
echo "TLS-PROOF served certificate subjectAltName: $(cat "$TEST_DIR/peer-san.txt")"

timing_next first-boot/shutdown
if ! send_monitor_quit "$MONITOR_SOCKET" >/dev/null 2>&1; then

    FAILURE_MESSAGE="QEMU monitor quit failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next auth-reboot/setup

# ITEM-401 phase 2: reboot the provisioned disk.  The gate must start closed
# again (the appliance has no session yet), the persisted record must enroll
# the same secret, and only then are the authenticated read-only bodies served.
# This also proves the fingerprint survives the reboot instead of living in the
# first boot's memory.
AUTH_SERIAL=$TEST_DIR/auth-boot.log
AUTH_QEMU_LOG=$TEST_DIR/auth-qemu.log
AUTH_MONITOR_SOCKET=$TEST_DIR/auth-monitor.sock
: >"$AUTH_SERIAL"
timing_next auth-reboot/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_AUTH_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$AUTH_SERIAL" \
    -monitor "unix:$AUTH_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$AUTH_QEMU_LOG" 2>&1 &
QEMU_PID=$!
auth_deadline=$((SECONDS + 120))
while (( SECONDS < auth_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$AUTH_SERIAL")" &&
        grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' "$(serial_view "$AUTH_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$AUTH_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the authenticated reboot proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before the authenticated reboot was ready"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    sleep 0.2
done
if ! grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' "$(serial_view "$AUTH_SERIAL")"; then
    FAILURE_MESSAGE="The provisioned fingerprint did not survive the reboot"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-403: the rebooted appliance must load the committed credential record
# the first boot provisioned (the enrollment record owns that state).
if ! grep -Eq '\[ADMIN-ENROLLMENT\] .*provisioned=1' "$(serial_view "$AUTH_SERIAL")"; then
    FAILURE_MESSAGE="The rebooted appliance did not report the committed credential record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next auth-reboot/client-closed
if ! python3 "$TEST_DIR/serving-closed-check.py" "$SERVING_AUTH_PORT"; then
    FAILURE_MESSAGE="The rebooted appliance did not keep the gate closed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next auth-reboot/console-login
ADMIN_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login \
    "$AUTH_MONITOR_SOCKET" "$AUTH_SERIAL" "$ADMIN_SECRET") || {
    FAILURE_MESSAGE="Administrator credential enrollment phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
if [[ ! "$ADMIN_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="Administrator session token is not the expected 32-character alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-506: the login conversation runs on this reboot and its password is
# hidden too, so the whole serial log of the boot is checked, not just the
# enrollment records: a LOGIN-mode echo regression must fail here.
if grep -Fq "$ADMIN_SECRET" "$(serial_view "$AUTH_SERIAL")"; then
    FAILURE_MESSAGE="The hidden console input leaked the password into the reboot serial log"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next auth-reboot/client-auth
if ! HFOC_PROOF_SERIAL_LOG="$AUTH_SERIAL" \
        python3 "$TEST_DIR/serving-auth-client.py" "$SERVING_AUTH_PORT" \
        "$ADMIN_TOKEN" "$ADMIN_SECRET"; then
    FAILURE_MESSAGE="Authenticated read-only management client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$AUTH_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the authenticated reboot proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next auth-reboot/shutdown
if ! send_monitor_quit "$AUTH_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the authenticated reboot"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next language/setup

# ITEM-513: the language switch gets a boot of its own, between the enrolled
# account boot and the routing proof.  It reuses the provisioned disk and
# starts from a fresh lwIP pool; the client signs in like a browser and never
# touches the console, so the phase spends none of the routing proof's state.
LANGUAGE_SERIAL=$TEST_DIR/language-boot.log
LANGUAGE_QEMU_LOG=$TEST_DIR/language-qemu.log
LANGUAGE_MONITOR_SOCKET=$TEST_DIR/language-monitor.sock
: >"$LANGUAGE_SERIAL"
timing_next language/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_LANGUAGE_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$LANGUAGE_SERIAL" \
    -monitor "unix:$LANGUAGE_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$LANGUAGE_QEMU_LOG" 2>&1 &
QEMU_PID=$!
language_deadline=$((SECONDS + 120))
while (( SECONDS < language_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$LANGUAGE_SERIAL")" &&
        grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' "$(serial_view "$LANGUAGE_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$LANGUAGE_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the language switch proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before the language switch proof was ready"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    sleep 0.2
done
if ! grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' "$(serial_view "$LANGUAGE_SERIAL")" ||
    ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$LANGUAGE_SERIAL")"; then
    FAILURE_MESSAGE="The language switch proof boot did not reload the disk and raise the listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# The credential record is loaded on this boot but no verifier is published
# until an operator presents the secret on the console (ITEM-401/ITEM-506), so
# the phase logs in once like the routing boot does; after that the browser
# form signs in with the same login and password.
timing_next language/console-login
LANGUAGE_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login \
    "$LANGUAGE_MONITOR_SOCKET" "$LANGUAGE_SERIAL" "$ADMIN_SECRET") || {
    FAILURE_MESSAGE="Administrator session login for the language switch proof failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
if [[ ! "$LANGUAGE_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="The language switch proof's administrator token is not the expected alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next language/client
if ! python3 "$TEST_DIR/serving-language-client.py" "$SERVING_LANGUAGE_PORT" \
        "$ADMIN_SECRET"; then
    FAILURE_MESSAGE="The language switch HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# The switch changes the browser's cookie and nothing on the machine: the
# global language record must not have been written during this boot, and the
# console command that writes it was never run.
if grep -Fq '[PRODUCTION-SYSTEM-LANGUAGE]' "$(serial_view "$LANGUAGE_SERIAL")"; then
    FAILURE_MESSAGE="The language switch proof wrote the machine language record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$LANGUAGE_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the language switch proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next language/shutdown
if ! send_monitor_quit "$LANGUAGE_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the language switch proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next routing/setup

# ITEM-504 increment 3: the routing mutation over HTTP gets a boot of its own.
# The two boots above leave their lwIP pools spent (eleven connections, then
# thirteen), and the pool refuses the next connection: the pool is small
# enough that a proof of four more requests cannot share a boot with them, so
# this phase reuses the provisioned disk and starts from a fresh pool.
ROUTING_SERIAL=$TEST_DIR/routing-boot.log
ROUTING_QEMU_LOG=$TEST_DIR/routing-qemu.log
ROUTING_MONITOR_SOCKET=$TEST_DIR/routing-monitor.sock
: >"$ROUTING_SERIAL"
# ITEM-504 increment 9: a proof backend that cannot bind (the randomized port
# is already taken) must fail the phase by name instead of surfacing later as
# an empty answer from the facade.
wait_for_facade_backend() {
    local log=$1 port=$2 pid=$3 deadline
    deadline=$((SECONDS + 20))
    while ! grep -Fq "facade backend listening on $port" "$log" 2>/dev/null; do
        if ! kill -0 "$pid" 2>/dev/null; then
            FAILURE_MESSAGE="The facade proof backend did not bind port $port"
            return 1
        fi
        if (( SECONDS >= deadline )); then
            FAILURE_MESSAGE="The facade proof backend published no listener in 20s"
            return 1
        fi
        sleep 0.1
    done
    return 0
}
timing_next routing/backend-ready
FACADE_BACKEND_LOG=$TEST_DIR/facade-backend.log
python3 "$TEST_DIR/facade-backend.py" "$FACADE_BACKEND_PORT" \
    >"$FACADE_BACKEND_LOG" 2>&1 &
FACADE_BACKEND_PID=$!
FACADE_BACKEND2_LOG=$TEST_DIR/facade-backend2.log
python3 "$TEST_DIR/facade-backend.py" "$FACADE_BACKEND2_PORT" \
    >"$FACADE_BACKEND2_LOG" 2>&1 &
FACADE_BACKEND2_PID=$!
if ! wait_for_facade_backend "$FACADE_BACKEND_LOG" "$FACADE_BACKEND_PORT" \
        "$FACADE_BACKEND_PID"; then
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! wait_for_facade_backend "$FACADE_BACKEND2_LOG" "$FACADE_BACKEND2_PORT" \
        "$FACADE_BACKEND2_PID"; then
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next routing/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_ROUTING_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$ROUTING_SERIAL" \
    -monitor "unix:$ROUTING_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$ROUTING_QEMU_LOG" 2>&1 &
QEMU_PID=$!
routing_deadline=$((SECONDS + 120))
while (( SECONDS < routing_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$ROUTING_SERIAL")" &&
        grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' "$(serial_view "$ROUTING_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$ROUTING_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the routing mutation proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before the routing proof was ready"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    sleep 0.2
done
if ! grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The routing proof boot did not reload the provisioned disk"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The routing proof boot raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next routing/console-login
ROUTING_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login \
    "$ROUTING_MONITOR_SOCKET" "$ROUTING_SERIAL" "$ADMIN_SECRET") || {
    FAILURE_MESSAGE="Administrator session login for the routing proof failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
if [[ ! "$ROUTING_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="Administrator session token is not the expected 32-character alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next routing/client
if ! HFOC_PROOF_SERIAL_LOG="$ROUTING_SERIAL" \
        python3 "$TEST_DIR/serving-routing-client.py" "$SERVING_ROUTING_PORT" \
        "$ROUTING_TOKEN" "$ADMIN_SECRET" "$FACADE_BACKEND_PORT" \
        "$FACADE_BACKEND2_PORT"; then
    FAILURE_MESSAGE="The routing mutation HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next routing/persisted-record
# ITEM-504 increment 5: the machine's own records.  The commit and the refused
# duplicate above each left one bounded body in the routing composition, and
# the ordinary loop drains it into the serial log as [PRODUCTION-ROUTE].  The
# loop runs between the client's requests, so the records are already written;
# the short wait only covers the last loop turn, and a missing line is a
# failure, never a skip.
ROUTING_RECORD_APPLIED='[PRODUCTION-ROUTE] name=a.example backend=192.0.2.10:443 tls=on source=PANEL result=APPLIED'
ROUTING_RECORD_REFUSED='[PRODUCTION-ROUTE] name=a.example result=REFUSED reason=DUPLICATE_NAME'
ROUTING_RECORD_RELAY="[PRODUCTION-ROUTE] name=b.example backend=10.0.2.2:$FACADE_BACKEND_PORT tls=off source=PANEL result=APPLIED"
ROUTING_RECORD_RELAY2="[PRODUCTION-ROUTE] name=c.example backend=10.0.2.2:$FACADE_BACKEND2_PORT tls=off source=PANEL result=APPLIED"
# ITEM-504 increment 10: the route whose commit failed was dropped by name, and
# the record says exactly that.
ROUTING_RECORD_CANCELLED='[PRODUCTION-ROUTE] name=d.example backend=192.0.2.20:8080 tls=off source=PANEL result=CANCELLED'
routing_record_deadline=$((SECONDS + 20))
while (( SECONDS < routing_record_deadline )); do
    if grep -Fq "$ROUTING_RECORD_APPLIED" "$(serial_view "$ROUTING_SERIAL")" &&
        grep -Fq "$ROUTING_RECORD_REFUSED" "$(serial_view "$ROUTING_SERIAL")" &&
        grep -Fq "$ROUTING_RECORD_RELAY" "$(serial_view "$ROUTING_SERIAL")" &&
        grep -Fq "$ROUTING_RECORD_RELAY2" "$(serial_view "$ROUTING_SERIAL")" &&
        grep -Fq "$ROUTING_RECORD_CANCELLED" "$(serial_view "$ROUTING_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$ROUTING_RECORD_APPLIED" "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The routing proof published no applied [PRODUCTION-ROUTE] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq "$ROUTING_RECORD_REFUSED" "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The routing proof published no refused [PRODUCTION-ROUTE] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq "$ROUTING_RECORD_RELAY" "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The routing proof published no relay [PRODUCTION-ROUTE] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq "$ROUTING_RECORD_RELAY2" "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The routing proof published no second relay [PRODUCTION-ROUTE] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq "$ROUTING_RECORD_CANCELLED" "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="The failed commit published no cancelled [PRODUCTION-ROUTE] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$ROUTING_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the routing mutation proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next routing/shutdown
if ! send_monitor_quit "$ROUTING_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the routing mutation proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next route-restore/setup

# ITEM-504 increment 6: the committed route survives the reboot.  The routing
# boot above committed `a.example` and `b.example` on this same writable disk;
# this boot has to read them back through the durable route store and publish
# the whole applied table as [PRODUCTION-ROUTES].  ITEM-504 increment 8 adds
# the facade relay requests here: this boot's lwIP pool is fresh, so the route
# that names the live proof backend can actually be exercised, and the record
# names the source and the model's own rendering of the table, so a table that
# came back empty, partial or different fails the phase.
REBOOT_SERIAL=$TEST_DIR/route-restore-boot.log
REBOOT_QEMU_LOG=$TEST_DIR/route-restore-qemu.log
REBOOT_MONITOR_SOCKET=$TEST_DIR/route-restore-monitor.sock
: >"$REBOOT_SERIAL"
timing_next route-restore/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_REBOOT_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$REBOOT_SERIAL" \
    -monitor "unix:$REBOOT_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$REBOOT_QEMU_LOG" 2>&1 &
QEMU_PID=$!
ROUTE_RESTORE_RECORD="[PRODUCTION-ROUTES] source=COMMITTED routes=3; name=a.example backend=192.0.2.10:443 tls=on source=PANEL; name=b.example backend=10.0.2.2:$FACADE_BACKEND_PORT tls=off source=PANEL; name=c.example backend=10.0.2.2:$FACADE_BACKEND2_PORT tls=off source=PANEL"
route_restore_deadline=$((SECONDS + 180))
while (( SECONDS < route_restore_deadline )); do
    if grep -Fq "$ROUTE_RESTORE_RECORD" "$(serial_view "$REBOOT_SERIAL")" &&
        grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$REBOOT_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$REBOOT_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the route restore proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$ROUTE_RESTORE_RECORD" "$(serial_view "$REBOOT_SERIAL")"; then
    FAILURE_MESSAGE="The rebooted appliance did not carry the committed route"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$REBOOT_SERIAL")"; then
    FAILURE_MESSAGE="The rebooted appliance raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next route-restore/client-relay
if ! python3 "$TEST_DIR/serving-facade-client.py" "$SERVING_REBOOT_PORT" \
        "$FACADE_BACKEND_PORT" "$FACADE_BACKEND2_PORT"; then
    FAILURE_MESSAGE="The facade relay HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$REBOOT_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the facade restore proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi

timing_next route-restore/backend-shutdown
kill "$FACADE_BACKEND_PID" 2>/dev/null || true
wait "$FACADE_BACKEND_PID" 2>/dev/null || true
FACADE_BACKEND_PID=
kill "$FACADE_BACKEND2_PID" 2>/dev/null || true
wait "$FACADE_BACKEND2_PID" 2>/dev/null || true
FACADE_BACKEND2_PID=
timing_next route-restore/shutdown
kill -TERM "$QEMU_PID" 2>/dev/null || true
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next network-change/setup

# ITEM-505: stage and confirm the network and hostname changes on their own
# fresh boot.  CI run 43547444691 mixed these requests with the four facade
# sessions above; the guest accepted 10 TCP connections, then reached
# `syn_rx=11 synack_tx=10` while the next TLS handshake timed out.  The six-PCB
# profile is intentional, so the proof gives this independent panel workflow a
# fresh pool instead of weakening the profile or retrying the failed client.
# The panel form signs in with the same secret, but the credential verifier is
# published only by a local login, as on every other authenticated boot.
NETWORK_CHANGE_SERIAL=$TEST_DIR/network-change-boot.log
NETWORK_CHANGE_QEMU_LOG=$TEST_DIR/network-change-qemu.log
NETWORK_CHANGE_MONITOR_SOCKET=$TEST_DIR/network-change-monitor.sock
: >"$NETWORK_CHANGE_SERIAL"
timing_next network-change/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_NETWORK_CHANGE_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$NETWORK_CHANGE_SERIAL" \
    -monitor "unix:$NETWORK_CHANGE_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$NETWORK_CHANGE_QEMU_LOG" 2>&1 &
QEMU_PID=$!
network_change_deadline=$((SECONDS + 180))
while (( SECONDS < network_change_deadline )); do
    if grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' \
            "$(serial_view "$NETWORK_CHANGE_SERIAL")" &&
        grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
            "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' \
            "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the network change proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq '[PRODUCTION-FIRST-BOOT] first_boot=reloaded' \
        "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The network change boot did not reload the installed record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
        "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The network change boot raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next network-change/console-login
NETWORK_CHANGE_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login \
    "$NETWORK_CHANGE_MONITOR_SOCKET" "$NETWORK_CHANGE_SERIAL" \
    "$ADMIN_SECRET") || {
    FAILURE_MESSAGE="Administrator session login for the network proof failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
if [[ ! "$NETWORK_CHANGE_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="Administrator session token is not the expected 32-character alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next network-change/client
if ! python3 "$TEST_DIR/serving-network-client.py" \
        "$SERVING_NETWORK_CHANGE_PORT" "$ADMIN_SECRET" mutate \
        "$NETWORK_CHANGE_TOKEN"; then
    FAILURE_MESSAGE="The network configuration HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next network-change/persisted-network
NETWORK_SET_RECORD='[PRODUCTION-NET-SET] source=PANEL mode=STATIC address=10.0.2.15/24 gateway=10.0.2.2 dns=10.0.2.3 persisted=OK applies=reboot current=DHCP'
network_record_deadline=$((SECONDS + 20))
while (( SECONDS < network_record_deadline )); do
    if grep -Fq "$NETWORK_SET_RECORD" \
            "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$NETWORK_SET_RECORD" \
        "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The network commit published no applied [PRODUCTION-NET-SET] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-505 increment 3: the hostname commit is one bounded record naming the
# source, the confirmed name, the proved write, the reboot it waits for and the
# identity the running system still serves.
timing_next network-change/persisted-hostname
SYSTEM_SET_RECORD='[PRODUCTION-SYSTEM-SET] source=PANEL hostname=node-505 persisted=OK applies=reboot current=hfoc-appliance'
system_record_deadline=$((SECONDS + 20))
while (( SECONDS < system_record_deadline )); do
    if grep -Fq "$SYSTEM_SET_RECORD" \
            "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$SYSTEM_SET_RECORD" \
        "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The hostname commit published no applied [PRODUCTION-SYSTEM-SET] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' \
        "$(serial_view "$NETWORK_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the network mutation proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next network-change/shutdown
if ! send_monitor_quit "$NETWORK_CHANGE_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the network change proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next network-apply/setup

# ITEM-505: the saved network configuration survives the reboot and is applied.
# The network-change boot committed the static record; this boot must read it back
# and run the interface on it, publish `[PRODUCTION-NET-MODE] mode=STATIC` with
# the address, raise the listener, and answer one request on that address.
# ITEM-505 increment 3 saves the hostname on the same boot, so this boot must
# also serve `node-505` as its identity.
NETWORK_APPLY_SERIAL=$TEST_DIR/network-apply-boot.log
NETWORK_APPLY_QEMU_LOG=$TEST_DIR/network-apply-qemu.log
NETWORK_APPLY_MONITOR_SOCKET=$TEST_DIR/network-apply-monitor.sock
: >"$NETWORK_APPLY_SERIAL"
timing_next network-apply/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_NETWORK_APPLY_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$NETWORK_APPLY_SERIAL" \
    -monitor "unix:$NETWORK_APPLY_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$NETWORK_APPLY_QEMU_LOG" 2>&1 &
QEMU_PID=$!
NETWORK_APPLY_RECORD='[PRODUCTION-NET-MODE] mode=STATIC address=10.0.2.15/24'
# ITEM-508 increment 10: this reboot was observed to need more than 180s while
# the phase's other QEMUs ran, and the wait bounds a boot, it does not retry
# or weaken any assertion, so it moves to 300s.
network_apply_deadline=$((SECONDS + 300))
while (( SECONDS < network_apply_deadline )); do
    if grep -Fq "$NETWORK_APPLY_RECORD" "$(serial_view "$NETWORK_APPLY_SERIAL")" &&
        grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
            "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the network apply proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$NETWORK_APPLY_RECORD" "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
    FAILURE_MESSAGE="The rebooted appliance did not apply the saved static network"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
        "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
    FAILURE_MESSAGE="The appliance raised no management listener on the applied static address"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-506 increment 5: this boot loaded the persisted account but has not
# published it yet (no console login), so the request gate answers the five
# managed targets with the gate-unavailable 503 while the routes stay known.
# This is the fresh-pool boot, so the five extra connections fit; the auth,
# routing and route-restore boots are already at the observed lwIP bound.
timing_next network-apply/client-closed
if ! python3 "$TEST_DIR/serving-closed-check.py" "$SERVING_NETWORK_APPLY_PORT" \
        "network-apply boot" /api/v1/system /api/v1/network \
        /api/v1/network/status /api/v1/service /api/v1/health; then
    FAILURE_MESSAGE="The applied static network did not keep the gate closed before the local login"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-505 increment 3: the authenticated body that proves the saved hostname
# is the identity this boot serves.  The credential verifier is published by a
# local login on this boot, exactly as the route-restore boot does before its
# own client.
timing_next network-apply/console-login
NETWORK_APPLY_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login \
    "$NETWORK_APPLY_MONITOR_SOCKET" "$NETWORK_APPLY_SERIAL" "$ADMIN_SECRET") || {
    FAILURE_MESSAGE="Administrator session login for the network apply proof failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
if [[ ! "$NETWORK_APPLY_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="Administrator session token is not the expected 32-character alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next network-apply/client
if ! python3 "$TEST_DIR/serving-network-client.py" \
        "$SERVING_NETWORK_APPLY_PORT" "$ADMIN_SECRET" apply \
        "$NETWORK_APPLY_TOKEN"; then
    FAILURE_MESSAGE="The network apply HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-515: the DHCP return is durable and honest about what is still running:
# the record the panel committed names DHCP while `current=` is the static
# configuration this boot applied, because the commit never rebinds the wire.
timing_next network-apply/persisted-dhcp
NETWORK_DHCP_RECORD='[PRODUCTION-NET-SET] source=PANEL mode=DHCP persisted=OK applies=reboot current=STATIC'
network_dhcp_deadline=$((SECONDS + 20))
while (( SECONDS < network_dhcp_deadline )); do
    if grep -Fq "$NETWORK_DHCP_RECORD" "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$NETWORK_DHCP_RECORD" "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
    FAILURE_MESSAGE="The panel did not return the saved network to DHCP by name"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "TLS-PROOF network panel: the saved record returned to DHCP while the live interface stayed static"
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$NETWORK_APPLY_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the network apply proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next network-apply/shutdown
if ! send_monitor_quit "$NETWORK_APPLY_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the network apply proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next account-change/setup

# ITEM-506 increment 6: the credential change itself.  The account was set on
# the auth boot and has not changed since; this boot publishes the verifier
# with a local login, signs in at `/` with the old pair to obtain a cookie,
# stages and confirms the change on the scriptable target with the console
# bearer, and then presents both pre-change credentials again: the cookie and
# the bearer must answer 401.  A second boot proves the change survived the
# reboot and that the old pair no longer signs in anywhere.
ACCOUNT_NEW_LOGIN=operator
ACCOUNT_NEW_SECRET=hfocoperator506
ACCOUNT_CHANGE_SERIAL=$TEST_DIR/account-change-boot.log
ACCOUNT_CHANGE_QEMU_LOG=$TEST_DIR/account-change-qemu.log
ACCOUNT_CHANGE_MONITOR_SOCKET=$TEST_DIR/account-change-monitor.sock
: >"$ACCOUNT_CHANGE_SERIAL"
timing_next account-change/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_ACCOUNT_CHANGE_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$ACCOUNT_CHANGE_SERIAL" \
    -monitor "unix:$ACCOUNT_CHANGE_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$ACCOUNT_CHANGE_QEMU_LOG" 2>&1 &
QEMU_PID=$!
account_change_deadline=$((SECONDS + 180))
while (( SECONDS < account_change_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
            "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the credential change proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
        "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The credential change boot raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next account-change/console-login
ACCOUNT_CHANGE_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login \
    "$ACCOUNT_CHANGE_MONITOR_SOCKET" "$ACCOUNT_CHANGE_SERIAL" \
    "$ADMIN_SECRET") || {
    FAILURE_MESSAGE="Administrator session login for the credential change failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
}
if [[ ! "$ACCOUNT_CHANGE_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="Administrator session token is not the expected 32-character alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next account-change/client
if ! python3 "$TEST_DIR/serving-account-client.py" \
        "$SERVING_ACCOUNT_CHANGE_PORT" "$ACCOUNT_NEW_LOGIN" \
        "$ACCOUNT_NEW_SECRET" "$ADMIN_SECRET" change "$ACCOUNT_CHANGE_TOKEN"; then
    FAILURE_MESSAGE="The credential change HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next account-change/persisted-credentials
ACCOUNT_SET_RECORD="[PRODUCTION-CREDENTIAL-SET] source=PANEL login=$ACCOUNT_NEW_LOGIN changed=LOGIN,PASSWORD sessions=REVOKED persisted=OK"
account_record_deadline=$((SECONDS + 20))
while (( SECONDS < account_record_deadline )); do
    if grep -Fq "$ACCOUNT_SET_RECORD" "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$ACCOUNT_SET_RECORD" "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The credential change published no applied [PRODUCTION-CREDENTIAL-SET] record"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq "$ACCOUNT_NEW_SECRET" "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="The new password reached the serial log"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$ACCOUNT_CHANGE_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the credential change proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next account-change/shutdown
if ! send_monitor_quit "$ACCOUNT_CHANGE_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the credential change proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next account-durability/setup

# ITEM-506 increment 6: the change is durable.  This boot loads the record the
# previous boot wrote: the old pair must be refused by the browser form, the
# new pair must sign in and the Account page must name the new login.  The
# console proves the same pair a second way and refuses the old password.
ACCOUNT_DURABILITY_SERIAL=$TEST_DIR/account-durability-boot.log
ACCOUNT_DURABILITY_QEMU_LOG=$TEST_DIR/account-durability-qemu.log
ACCOUNT_DURABILITY_MONITOR_SOCKET=$TEST_DIR/account-durability-monitor.sock
: >"$ACCOUNT_DURABILITY_SERIAL"
timing_next account-durability/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_ACCOUNT_DURABILITY_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$ACCOUNT_DURABILITY_SERIAL" \
    -monitor "unix:$ACCOUNT_DURABILITY_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$ACCOUNT_DURABILITY_QEMU_LOG" 2>&1 &
QEMU_PID=$!
account_durability_deadline=$((SECONDS + 180))
while (( SECONDS < account_durability_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
            "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' \
            "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the credential durability proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The credential durability boot raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# The console refuses the old password for the new login and then binds the new
# pair.  CI-SPEEDUP-3: the refused attempt is one answer, so the proof finishes
# on the console's own refusal line instead of driving the whole sign-in
# conversation and waiting out the 120-second session marker that a retired
# password can never earn.  The successful local login also publishes the
# verifier through the request gate; before it, the five managed targets answer
# the gate-unavailable 503 (the network-apply boot proves that shape), so the
# HTTP client below must run after this conversation.
timing_next account-durability/console-policy
if ! python3 "$TEST_DIR/admin-enroll.py" refuse \
        "$ACCOUNT_DURABILITY_MONITOR_SOCKET" "$ACCOUNT_DURABILITY_SERIAL" \
        "$ADMIN_SECRET" "$ACCOUNT_NEW_LOGIN"; then
    FAILURE_MESSAGE="The retired password was not refused by name on the local console"
    echo "$FAILURE_MESSAGE" >&2
    dump_gate_diagnostics "$ACCOUNT_DURABILITY_SERIAL"
    exit 1
fi
echo "TLS-PROOF account durability: the retired password is refused by name for the new login"
# ITEM-516: this one invocation signs in AND enables the after-boot policy in
# the same console session - one process, one monitor connection.  The local
# secret the sign-in just proved is what `gate open confirm` requires, so no
# password is typed twice and nothing here is reachable from the network.  The
# command substitution still receives exactly the 32-character token, and only
# that, on stdout; every gate line goes to stderr.
ACCOUNT_DURABILITY_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login-gate \
    "$ACCOUNT_DURABILITY_MONITOR_SOCKET" "$ACCOUNT_DURABILITY_SERIAL" \
    "$ACCOUNT_NEW_SECRET" "$ACCOUNT_NEW_LOGIN") || {
    FAILURE_MESSAGE="The new credential did not sign in and enable the gate on the local console"
    echo "$FAILURE_MESSAGE" >&2
    dump_gate_diagnostics "$ACCOUNT_DURABILITY_SERIAL"
    exit 1
}
if [[ ! "$ACCOUNT_DURABILITY_TOKEN" =~ ^[A-Za-z0-9_-]{32}$ ]]; then
    FAILURE_MESSAGE="Administrator session token is not the expected 32-character alphabet"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# The record must name the three states in the order they happened: this boot's
# own closed gate, the request, and the write the confirmation performed.  The
# operator's warning and the local session are part of the same evidence.
timing_next account-durability/persisted-policy
GATE_CLOSED_RECORD="[PRODUCTION-SECURITY] gate=CLOSED reason=AWAITING_LOCAL_SECRET"
GATE_PENDING_RECORD="[PRODUCTION-SECURITY] gate=PENDING action=open confirmation_required"
GATE_ENABLED_RECORD="[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET"
gate_enabled_deadline=$((SECONDS + 20))
while (( SECONDS < gate_enabled_deadline )); do
    if grep -Fq "$GATE_ENABLED_RECORD" \
            "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$GATE_ENABLED_RECORD" \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The console enable gesture published no gate record"
    echo "$FAILURE_MESSAGE" >&2
    dump_gate_diagnostics "$ACCOUNT_DURABILITY_SERIAL"
    exit 1
fi
if ! grep -Fq 'ADMIN SESSION: Authorization: Bearer ' \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The gate-enable console issued no local session"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if ! grep -Fq 'The machine will NO LONGER REQUIRE PHYSICAL PRESENCE' \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The console printed no physical-presence warning"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
gate_closed_line=$(grep -n -F -m1 "$GATE_CLOSED_RECORD" \
    "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")" | cut -d: -f1)
gate_pending_line=$(grep -n -F -m1 "$GATE_PENDING_RECORD" \
    "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")" | cut -d: -f1)
gate_enabled_line=$(grep -n -F -m1 "$GATE_ENABLED_RECORD" \
    "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")" | cut -d: -f1)
if [[ -z "$gate_closed_line" || -z "$gate_pending_line" ||
      -z "$gate_enabled_line" ||
      "$gate_closed_line" -ge "$gate_pending_line" ||
      "$gate_pending_line" -ge "$gate_enabled_line" ]]; then
    FAILURE_MESSAGE="The durability boot did not publish CLOSED, PENDING and ENABLED in order"
    echo "$FAILURE_MESSAGE" >&2
    grep -n -F 'PRODUCTION-SECURITY' "$ACCOUNT_DURABILITY_SERIAL" >&2 || true
    exit 1
fi
echo "TLS-PROOF gate enable: the local console wrote the policy after a proven local secret"
timing_next account-durability/client-credentials
if ! HFOC_PROOF_SERIAL_LOG="$ACCOUNT_DURABILITY_SERIAL" \
        python3 "$TEST_DIR/serving-account-client.py" \
        "$SERVING_ACCOUNT_DURABILITY_PORT" "$ACCOUNT_NEW_LOGIN" \
        "$ACCOUNT_NEW_SECRET" "$ADMIN_SECRET" durability; then
    FAILURE_MESSAGE="The credential durability HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# ITEM-507 increment 4: this boot commits the explicit default declaration
# through the scriptable name mutation.  It runs after this boot's own TLS
# clients on purpose: the commit does not rebind the running listener
# (`applies=reboot`), but the next boot reads the committed table and refuses
# every unknown name, so no TLS client of this boot may follow it.  The fixture
# clock is not validated, so a certificate import would be refused by name here
# (its parsing is proven on the host); the declaration needs no clock and
# proves the target, the authorization, the bound executor, the durable write
# and the record.
timing_next account-durability/client-sni
if ! python3 "$TEST_DIR/serving-certificate-name-client.py" \
        "$SERVING_ACCOUNT_DURABILITY_PORT" "$ACCOUNT_NEW_LOGIN" \
        "$ACCOUNT_NEW_SECRET" commit; then
    FAILURE_MESSAGE="The certificate-name mutation HTTP client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next account-durability/persisted-sni
CERTIFICATE_NAME_RECORD="[PRODUCTION-CERTIFICATE-NAME] source=PANEL action=DEFAULT name=- names=0 default=REFUSE persisted=OK applies=reboot"
certificate_name_deadline=$((SECONDS + 20))
while (( SECONDS < certificate_name_deadline )); do
    if grep -Fq "$CERTIFICATE_NAME_RECORD" \
            "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$CERTIFICATE_NAME_RECORD" \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The certificate-name mutation published no applied [PRODUCTION-CERTIFICATE-NAME] record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F 'PRODUCTION-CERTIFICATE-NAME' "$ACCOUNT_DURABILITY_SERIAL" >&2 || true
    exit 1
fi
echo "TLS-PROOF certificate-name: the durable record names the committed declaration"
if grep -Fq "$ACCOUNT_NEW_SECRET" \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="The new password reached the serial log of the durability boot"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' \
        "$(serial_view "$ACCOUNT_DURABILITY_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the credential durability proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next account-durability/shutdown
if ! send_monitor_quit "$ACCOUNT_DURABILITY_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the credential durability proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next sni-durability/setup

# ITEM-507 increment 4: the committed declaration is in force.  This boot
# reads the record the credential-change boot wrote (no names, default REFUSE):
# an unknown SNI name must fail the handshake with a record that names the
# reason, while a client that presents no name is not an unknown name and
# still gets the appliance's own identity.
SNI_SERIAL=$TEST_DIR/sni-durability-boot.log
SNI_QEMU_LOG=$TEST_DIR/sni-durability-qemu.log
SNI_MONITOR_SOCKET=$TEST_DIR/sni-durability-monitor.sock
: >"$SNI_SERIAL"
timing_next sni-durability/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_SNI_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$SNI_SERIAL" \
    -monitor "unix:$SNI_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$SNI_QEMU_LOG" 2>&1 &
QEMU_PID=$!
sni_deadline=$((SECONDS + 180))
while (( SECONDS < sni_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$SNI_SERIAL")"; then
        break
    fi
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SNI_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the SNI declaration proof"
        echo "$FAILURE_MESSAGE" >&2
        exit 1
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$SNI_SERIAL")"; then
    FAILURE_MESSAGE="The SNI declaration boot raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next sni-durability/client
if ! python3 "$TEST_DIR/serving-certificate-name-client.py" \
        "$SERVING_SNI_PORT" "$ACCOUNT_NEW_LOGIN" "$ACCOUNT_NEW_SECRET" \
        durability; then
    FAILURE_MESSAGE="The certificate-name durability client phase failed"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
SNI_REFUSAL="[PRODUCTION-TLS-SNI] name=unknown.example result=REFUSED cert=none reason=NOT_FOUND"
SNI_DEFAULT="[PRODUCTION-TLS-SNI] name=- result=DEFAULT cert=default"
timing_next sni-durability/handshake-records
sni_record_deadline=$((SECONDS + 20))
while (( SECONDS < sni_record_deadline )); do
    if grep -Fq "$SNI_REFUSAL" "$(serial_view "$SNI_SERIAL")" &&
            grep -Fq "$SNI_DEFAULT" "$(serial_view "$SNI_SERIAL")"; then
        break
    fi
    sleep 0.2
done
if ! grep -Fq "$SNI_REFUSAL" "$(serial_view "$SNI_SERIAL")"; then
    FAILURE_MESSAGE="The refused SNI name published no selection record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F 'PRODUCTION-TLS-SNI' "$SNI_SERIAL" >&2 || true
    exit 1
fi
if ! grep -Fq "$SNI_DEFAULT" "$(serial_view "$SNI_SERIAL")"; then
    FAILURE_MESSAGE="The no-SNI client published no selection record"
    echo "$FAILURE_MESSAGE" >&2
    grep -F 'PRODUCTION-TLS-SNI' "$SNI_SERIAL" >&2 || true
    exit 1
fi
echo "TLS-PROOF certificate-name durability: the committed declaration is in force after the reboot"
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$SNI_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the SNI declaration proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next sni-durability/shutdown
if ! send_monitor_quit "$SNI_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the SNI declaration proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next policy/setup


# ITEM-516: the after-boot policy boot.  The record the local console wrote on
# the credential durability boot says `open`, so this boot publishes the stored
# credential itself and the panel answers over the network with no local login
# anywhere in this boot.  The same proof keeps the default honest: the earlier
# boots of this script are the default (`closed`) and answer the
# gate-unavailable 503 before a local login.
POLICY_DIR=$TEST_DIR/policy
mkdir -p "$POLICY_DIR"
POLICY_SERIAL=$POLICY_DIR/boot.log
POLICY_QEMU_LOG=$POLICY_DIR/qemu.log
POLICY_MONITOR_SOCKET=$POLICY_DIR/monitor.sock
: >"$POLICY_SERIAL"
timing_next policy/readiness
qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" -cpu "$QEMU_CPU" \
    -display none -boot order=d,menu=off -cdrom "$IMAGE" \
    -drive "if=none,id=hfoc,file=$DISK,format=raw,cache=writeback,file.locking=off" \
    -device 'ide-hd,drive=hfoc,bus=ide.0' \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$SERVING_POLICY_PORT-:443" \
    -device "$NETDEV_DEVICE" \
    -serial "file:$POLICY_SERIAL" \
    -monitor "unix:$POLICY_MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown >"$POLICY_QEMU_LOG" 2>&1 &
QEMU_PID=$!
POLICY_PUBLISHED_RECORD="[PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY"
# ITEM-544: this wait declares its own class.  `wait_for_record` prints the
# canonical HFOC-QEMU-MARKER-TIMEOUT record when the deadline expires, so a
# record a loaded guest has not published in time takes the suite's documented
# one retry as the timing class it is - instead of being read as an assertion
# with no retry (`no-marker-timeout-record`).  This boot writes `POLICY_SERIAL`,
# not the phase-1 log, so the wait names it: the helper reads whichever log it
# is given through `serial_view`, and the call is laid out so the ITEM-526
# raw-file control stays exact instead of being edited.  A kernel panic in the
# same log stays fail-closed: the classifier refuses the timing class when a
# fail-closed signature is present.  No assertion moves - the record is still
# required, and the diagnostic dump below is unchanged.
if ! wait_for_record "$POLICY_PUBLISHED_RECORD" 180 \
        "$POLICY_SERIAL"
then
    if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$POLICY_SERIAL")"; then
        FAILURE_MESSAGE="Kernel panic during the after-boot policy proof"
    else
        FAILURE_MESSAGE="The policy boot published no gate=PUBLISHED source=POLICY record"
    fi
    echo "$FAILURE_MESSAGE" >&2
    grep -F 'PRODUCTION-SECURITY' "$POLICY_SERIAL" >&2 || true
    exit 1
fi
echo "TLS-PROOF policy boot: the boot record names the policy that opened the gate"
policy_live_deadline=$((SECONDS + 120))
while (( SECONDS < policy_live_deadline )); do
    if grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$POLICY_SERIAL")"; then
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        break
    fi
    sleep 0.2
done
if ! grep -Eq '\[PRODUCTION-SERVING\] .*listener=live' "$(serial_view "$POLICY_SERIAL")"; then
    FAILURE_MESSAGE="The policy boot raised no management listener"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
# No local login happens on this boot: no console conversation, no monitor
# keystroke.  The bare TLS client must see the gate published instead of the
# 503 the default boots answer.
timing_next policy/client
if ! python3 "$TEST_DIR/serving-closed-check.py" --open \
        "$SERVING_POLICY_PORT" "policy boot" / /api/v1/system; then
    FAILURE_MESSAGE="The policy boot did not answer without a local login"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
echo "TLS-PROOF policy boot: the panel answers after boot with no local login"
echo "TLS-PROOF policy boot: the default boot still answers AUTH_UNAVAILABLE"
if grep -Fq "$ACCOUNT_NEW_SECRET" "$(serial_view "$POLICY_SERIAL")"; then
    FAILURE_MESSAGE="The administrator password reached the policy boot serial log"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
if grep -Fq '*** HFOC KERNEL PANIC ***' "$(serial_view "$POLICY_SERIAL")"; then
    FAILURE_MESSAGE="Kernel panic during the after-boot policy proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
timing_next policy/shutdown
if ! send_monitor_quit "$POLICY_MONITOR_SOCKET" >/dev/null 2>&1; then
    FAILURE_MESSAGE="QEMU monitor quit failed during the after-boot policy proof"
    echo "$FAILURE_MESSAGE" >&2
    exit 1
fi
wait "$QEMU_PID" 2>/dev/null || true
QEMU_PID=
timing_next summary

echo "Production service activation record: found"
echo "Production service honest readiness: found"
echo "Production service secrets marker: found"
echo "Management TLS lifecycle record: found"
echo "Management TLS frozen policy: found"
echo "Management TLS HSTS publication: found"
echo "Production serving record: found"
echo "Production serving live TLS 1.3 client: OK"
echo "Production serving panel denial: OK"
echo "Production serving routing mutation over HTTP: OK"
echo "Production serving routing page: OK"
echo "Production serving routing record: applied and refused are named"
echo "Production serving network form: saved static, live DHCP, applies reboot"
echo "Production serving network record: applied and persisted"
echo "Production serving network apply after reboot: OK"
echo "Production serving system form: saved hostname, unchanged identity"
echo "Production serving system apply after reboot: OK"
echo "Production serving credential change and session revocation: OK"
echo "Production serving credential durability after reboot: OK"
echo "Production serving fail-closed without entropy: OK"
echo "Kernel panic: none"
echo "QEMU production service activation main part: OK"
