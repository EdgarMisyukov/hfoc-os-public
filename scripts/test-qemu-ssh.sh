#!/usr/bin/env bash
# ITEM-601: the production SSH endpoint on a QEMU fixture build.
#
# The fixture kernel seeds an in-memory host key and the committed test-only
# client key; it does not touch an owner volume.  This is a QEMU proof of the
# production listener, the SSHv2 transport and the real stock OpenSSH client,
# not a Toshiba hardware proof.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in qemu-system-x86_64 ssh ssh-keygen python3 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

IMAGE=${IMAGE:-build/ssh-qemu/hfoc-os-$(<VERSION)-ssh-qemu.iso}
QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_CPU=${QEMU_CPU:-max}
QEMU_SMP=${QEMU_SMP:-1}
CLIENT_KEY=${SSH_QEMU_CLIENT_KEY:-tests/fixtures/ssh/id_ecdsa}
READY_TIMEOUT=${SSH_QEMU_READY_TIMEOUT:-180}
[[ -f $IMAGE ]] || { echo "SSH QEMU image is missing: $IMAGE" >&2; exit 1; }
[[ -f $CLIENT_KEY ]] || { echo "SSH test client key is missing: $CLIENT_KEY" >&2; exit 1; }
test "$QEMU_MACHINE" = q35 || { echo "SSH QEMU proof requires q35" >&2; exit 1; }
test "$QEMU_SMP" = 1 || { echo "SSH QEMU proof requires one CPU" >&2; exit 1; }

TEST_DIR=$(mktemp -d)
SERIAL_LOG=$TEST_DIR/serial.log
QEMU_LOG=$TEST_DIR/qemu.log
QEMU_PID=
FAILURE_MESSAGE=

: >"$SERIAL_LOG"
: >"$QEMU_LOG"

save_artifacts() {
    local artifact_dir
    artifact_dir="build/test-artifacts/ssh-qemu-$(date -u +%Y%m%dT%H%M%SZ)-$$"
    mkdir -p "$artifact_dir"
    printf '%s\n' "${FAILURE_MESSAGE:-SSH QEMU proof failed}" \
        >"$artifact_dir/failure-reason.txt"
    cp -a "$SERIAL_LOG" "$QEMU_LOG" "$artifact_dir/" 2>/dev/null || true
    for extra in pty.out pty.err shell.out shell.err status.out status.err; do
        [[ -e $TEST_DIR/$extra ]] && cp -a "$TEST_DIR/$extra" "$artifact_dir/"
    done
    echo "SSH QEMU artifacts: $artifact_dir" >&2
}

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        save_artifacts
    fi
    if [[ -n $QEMU_PID ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null || true
        for _ in $(seq 1 30); do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$QEMU_PID" 2>/dev/null || true
    fi
    if [[ -n $QEMU_PID ]]; then
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

PORT=$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)

copy_key="$TEST_DIR/id_ecdsa"
cp "$CLIENT_KEY" "$copy_key"
chmod 600 "$copy_key"

qemu-system-x86_64 \
    -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
    -cpu "$QEMU_CPU" -display none -cdrom "$IMAGE" \
    -serial "file:$SERIAL_LOG" -monitor none \
    -netdev user,id=sshqemu0,hostfwd=tcp:127.0.0.1:"$PORT"-:22 \
    -device virtio-net-pci-non-transitional,netdev=sshqemu0,ioeventfd=off,\
mac=52:54:00:12:34:57,\
event_idx=off,indirect_desc=off,\
mq=off,mrg_rxbuf=off,ctrl_vq=off,ctrl_rx=off,ctrl_rx_extra=off,\
ctrl_vlan=off,ctrl_mac_addr=off,ctrl_guest_offloads=off,gso=off,\
guest_csum=off,csum=off,guest_tso4=off,guest_tso6=off,guest_ecn=off,\
guest_ufo=off,host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off,\
host_uso=off,guest_uso4=off,guest_uso6=off,guest_announce=off,\
hash=off,rss=off,guest_rsc_ext=off,vectors=0,x-disable-pcie=on \
    -no-reboot -no-shutdown >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!

hostkey=
deadline=$((SECONDS + READY_TIMEOUT))
while (( SECONDS < deadline )); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        FAILURE_MESSAGE="QEMU exited before the SSH listener became ready"
        break
    fi
    if grep -Fq '[SSH-QEMU] hostkey=' "$SERIAL_LOG" 2>/dev/null &&
       grep -Fq '[PRODUCTION-SSH] state=LISTENING' "$SERIAL_LOG" 2>/dev/null; then
        hostkey=$(sed -n 's/.*\[SSH-QEMU\] hostkey=\([A-Za-z0-9+/=]*\).*/\1/p' \
            "$SERIAL_LOG" | tail -1)
        [[ -n $hostkey ]] && break
    fi
    sleep 0.2
done
if [[ -z $hostkey ]]; then
    FAILURE_MESSAGE="the SSH listener did not publish a host key and LISTENING within ${READY_TIMEOUT}s"
    echo "$FAILURE_MESSAGE" >&2
    grep -E '\[PRODUCTION-SSH\]|\[SSH-QEMU\]|\[PRODUCTION-NET' "$SERIAL_LOG" >&2 || true
    exit 1
fi

printf '[127.0.0.1]:%s ecdsa-sha2-nistp256 %s\n' "$PORT" "$hostkey" \
    >"$TEST_DIR/known_hosts"

# A malformed pre-auth connection must release its single session slot and
# return to the listener without a use-after-free or a dead endpoint.
python3 - "$PORT" <<'PY'
import socket
import sys

sock = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=10)
sock.sendall(b"GARBAGE\r\n" + b"\x00\x00\x00\x00")
sock.close()
PY

run_ssh() {
    local command=$1
    shift
    timeout 30 ssh -F /dev/null -p "$PORT" -i "$copy_key" \
        -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$TEST_DIR/known_hosts" \
        -o StrictHostKeyChecking=yes \
        -o BatchMode=yes \
        -o KexAlgorithms=ecdh-sha2-nistp256 \
        -o Ciphers=aes128-gcm@openssh.com \
        -o MACs=hmac-sha2-256 \
        "$@" hfoc@127.0.0.1 "$command"
}

# The malformed pre-auth probe above is closed by the client; the adapter
# releases its single session slot asynchronously (the close/poll path), so a
# real client that connects in the same millisecond can see `reason=busy`.
# Retry the first real session a bounded number of times: the listener must
# still accept it, and a dead endpoint fails all three attempts.
status_exit=1
for attempt in 1 2 3; do
    set +e
    run_ssh 'hfoc status' >"$TEST_DIR/status.out" 2>"$TEST_DIR/status.err"
    status_exit=$?
    set -e
    (( status_exit == 0 )) && break
    (( attempt < 3 )) && sleep 1
done
printf 'ssh_exit=%s attempts=%s\n' "$status_exit" "$attempt"
cat "$TEST_DIR/status.out"
if (( status_exit != 0 )); then
    cat "$TEST_DIR/status.err" >&2
    FAILURE_MESSAGE="stock OpenSSH did not complete the production SSH handshake"
    exit 1
fi
grep -Fq '[PRODUCTION-SSH] state=LISTENING' "$TEST_DIR/status.out" || {
    FAILURE_MESSAGE="hfoc status did not name the production SSH state"
    exit 1
}
grep -Fq '[NET-STATUS]' "$TEST_DIR/status.out" || {
    FAILURE_MESSAGE="hfoc status did not include the network owner's record"
    exit 1
}
# Coexistence: the HTTPS panel listener on 443 and SSH on 22 are both live in
# the same boot.  The three-slot listener pool and its fourth-listener refusal
# are pinned by tests/lwip-tcp-host.c in the same host suite.
grep -Fq '[PANEL]' "$TEST_DIR/status.out" || {
    FAILURE_MESSAGE="hfoc status did not include the HTTPS panel record"
    exit 1
}
grep -Fq 'listener=live' "$TEST_DIR/status.out" || {
    FAILURE_MESSAGE="the HTTPS panel listener was not live beside SSH"
    exit 1
}

set +e
run_ssh 'hfoc network' >"$TEST_DIR/network.out" 2>"$TEST_DIR/network.err"
network_exit=$?
run_ssh 'hfoc journal' >"$TEST_DIR/journal.out" 2>"$TEST_DIR/journal.err"
journal_exit=$?
run_ssh 'hfoc diag' >"$TEST_DIR/diag.out" 2>"$TEST_DIR/diag.err"
diag_exit=$?
set -e
(( network_exit == 0 )) || {
    FAILURE_MESSAGE="hfoc network failed"
    cat "$TEST_DIR/network.err" >&2
    exit 1
}
grep -Fq '[NET-STATUS]' "$TEST_DIR/network.out" || {
    FAILURE_MESSAGE="hfoc network did not include the driver's record"
    exit 1
}
grep -Fq '[PANEL]' "$TEST_DIR/network.out" || {
    FAILURE_MESSAGE="hfoc network did not include the panel record"
    exit 1
}
grep -Fq '[PRODUCTION-SSH]' "$TEST_DIR/network.out" || {
    FAILURE_MESSAGE="hfoc network did not include the SSH readiness record"
    exit 1
}
(( journal_exit == 0 )) || {
    FAILURE_MESSAGE="hfoc journal failed"
    cat "$TEST_DIR/journal.err" >&2
    exit 1
}
grep -Fq 'BOOTLOG: state=' "$TEST_DIR/journal.out" || {
    FAILURE_MESSAGE="hfoc journal did not name the boot log state"
    exit 1
}
(( diag_exit == 0 )) || {
    FAILURE_MESSAGE="hfoc diag failed"
    cat "$TEST_DIR/diag.err" >&2
    exit 1
}
grep -Fq 'state=BEGIN' "$TEST_DIR/diag.out" || {
    FAILURE_MESSAGE="hfoc diag has no BEGIN record"
    exit 1
}
grep -Fq 'state=END' "$TEST_DIR/diag.out" || {
    FAILURE_MESSAGE="hfoc diag has no END record"
    exit 1
}
grep -Fq 'mode=STREAM' "$TEST_DIR/diag.out" || {
    FAILURE_MESSAGE="hfoc diag did not name the stream mode"
    exit 1
}
if grep -Fq 'SAVED' "$TEST_DIR/diag.out"; then
    FAILURE_MESSAGE="hfoc diag claimed a medium save"
    exit 1
fi

# ITEM-613: the same stream carries the public host key in OpenSSH form, split
# by the normal continuation rule.  Reassemble it and require the blob the SSH
# handshake used and its independently computed SHA256 fingerprint.
python3 - "$TEST_DIR/diag.out" "$hostkey" <<'HOSTKEY'
import base64
import hashlib
import pathlib
import re
import sys

path, hostkey = pathlib.Path(sys.argv[1]), sys.argv[2]
lines = []
for line in path.read_text(encoding="ascii", errors="replace").splitlines():
    if line.startswith("[DIAG+] ") and lines:
        lines[-1] += line[len("[DIAG+] "):]
    else:
        lines.append(line)
records = [line for line in lines if line.startswith("[PRODUCTION-SSH-HOSTKEY] ")]
if not records:
    raise SystemExit("hfoc diag has no [PRODUCTION-SSH-HOSTKEY] record")
fields = dict(re.findall(r"([a-z_]+)=([^ ]+)", records[-1]))
if fields.get("state") != "READY":
    raise SystemExit("the host-key record is not READY: " + records[-1])
if fields.get("blob") != hostkey:
    raise SystemExit("the exported blob is not the handshake host key")
blob = base64.b64decode(fields["blob"], validate=True)
fingerprint = "SHA256:" + base64.b64encode(
    hashlib.sha256(blob).digest()).decode("ascii").rstrip("=")
if fields.get("fingerprint") != fingerprint:
    raise SystemExit(
        f"the declared fingerprint {fields.get('fingerprint')} != {fingerprint}")
print("SSH QEMU host-key export: blob and fingerprint match the handshake")
HOSTKEY

# The collector is the operator's path: pinned known_hosts, private key file,
# and a verified CRC/count before the report is named valid.
COLLECTED=$(
    "$ROOT/scripts/hfoc-ssh-collect.sh" \
        --host 127.0.0.1 --port "$PORT" \
        --key "$copy_key" --known-hosts "$TEST_DIR/known_hosts" \
        --out "$TEST_DIR/collected" --command diag
)
[[ -s $COLLECTED ]] || {
    FAILURE_MESSAGE="the collector did not produce a diagnostic file"
    exit 1
}
grep -Fq 'verified=yes' "${COLLECTED%.txt}.status" || {
    FAILURE_MESSAGE="the collector did not verify the diag CRC/count"
    exit 1
}
grep -Fq 'session=' "${COLLECTED%.txt}.status" || {
    FAILURE_MESSAGE="the collector did not record the diag session"
    exit 1
}

set +e
run_ssh 'echo pwned' >"$TEST_DIR/refused.out" 2>"$TEST_DIR/refused.err"
refused_exit=$?
set -e
if (( refused_exit == 0 )) || grep -Fq 'pwned' "$TEST_DIR/refused.out"; then
    FAILURE_MESSAGE="the SSH endpoint executed an unknown command"
    exit 1
fi
grep -Fq 'refused' "$TEST_DIR/refused.out" || {
    FAILURE_MESSAGE="the unknown-command refusal body is missing"
    exit 1
}

# The non-exec requests are refused by name, not silently accepted.
run_ssh_raw() {
    timeout 20 ssh -F /dev/null -p "$PORT" -i "$copy_key" \
        -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$TEST_DIR/known_hosts" \
        -o StrictHostKeyChecking=yes \
        -o BatchMode=yes \
        -o KexAlgorithms=ecdh-sha2-nistp256 \
        -o Ciphers=aes128-gcm@openssh.com \
        -o MACs=hmac-sha2-256 \
        "$@"
}

set +e
run_ssh_raw -s hfoc@127.0.0.1 sftp >"$TEST_DIR/subsystem.out" 2>"$TEST_DIR/subsystem.err"
subsystem_exit=$?
run_ssh_raw -W 127.0.0.1:22 hfoc@127.0.0.1 </dev/null >"$TEST_DIR/forward.out" 2>"$TEST_DIR/forward.err"
forward_exit=$?
# ITEM-629/633: the enrolled owner gets a real pty, a real /bin/sh and job
# control.  One session runs the shell language checks and the Ctrl-Z/Ctrl-C,
# jobs/bg/fg/wait proof; the output is checked below and on failure kept in
# the artifacts.
if ! PORT="$PORT" SSH_KEY="$copy_key" \
        SSH_KNOWN_HOSTS="$TEST_DIR/known_hosts" \
        SSH_SHELL_OUT="$TEST_DIR/shell.out" \
        timeout 90 python3 scripts/test-ssh-jobs.py; then
    FAILURE_MESSAGE="the owner shell/job-control proof failed"
    cat "$TEST_DIR/shell.out" >&2 2>/dev/null || true
    exit 1
fi
for expected in 'hfoc$' 'shell-ok' '/tmp/shell-dir' 'file' 'script-ok' \
                'bg-done' 'Stopped' 'Running' 'job-done'; do
    grep -Fq -- "$expected" "$TEST_DIR/shell.out" || {
        FAILURE_MESSAGE="the owner shell output is missing: $expected"
        cat "$TEST_DIR/shell.out" >&2
        exit 1
    }
done

if (( subsystem_exit == 0 )); then
    FAILURE_MESSAGE="a subsystem request was accepted"
    exit 1
fi
if (( forward_exit == 0 )); then
    FAILURE_MESSAGE="a direct-tcpip forwarding request was accepted"
    exit 1
fi

# ITEM-634: a second shell must bind its own TTY after the first session has
# closed.  This is a shell session, not an exec request.
printf '%s\n' 'echo second-session-ok' 'exit' |
    timeout 60 ssh -F /dev/null -p "$PORT" -i "$copy_key" \
        -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$TEST_DIR/known_hosts" \
        -o StrictHostKeyChecking=yes \
        -o BatchMode=yes \
        -o KexAlgorithms=ecdh-sha2-nistp256 \
        -o Ciphers=aes128-gcm@openssh.com \
        -o MACs=hmac-sha2-256 \
        -tt hfoc@127.0.0.1 >"$TEST_DIR/second.out" 2>"$TEST_DIR/second.err" || {
    FAILURE_MESSAGE="the second owner shell session failed"
    cat "$TEST_DIR/second.out" >&2
    cat "$TEST_DIR/second.err" >&2
    exit 1
}
grep -Fq 'second-session-ok' "$TEST_DIR/second.out" || {
    FAILURE_MESSAGE="the second owner shell did not reach a prompt"
    cat "$TEST_DIR/second.out" >&2
    exit 1
}

# The second connection proves the one-session cleanup: the first session is
# closed before the second handshake, so the same listener accepts it again.
set +e
run_ssh 'hfoc status' >"$TEST_DIR/status2.out" 2>"$TEST_DIR/status2.err"
status2_exit=$?
set -e
(( status2_exit == 0 )) || {
    FAILURE_MESSAGE="a second SSH session was not accepted after the first closed"
    exit 1
}

echo "SSH QEMU production endpoint proof: ALL OK"
