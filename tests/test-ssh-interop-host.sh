#!/usr/bin/env bash
# ITEM-601: real stock-OpenSSH interop against the ported SSHv2 transport.
#
# This is a host proof, not a hardware acceptance: it starts the POSIX driver
# in tests/ssh-interop-server.c, pins the host key it prints, and drives it
# with the system OpenSSH client.  The production endpoint is separate; the
# same wire/crypto/transport modules are used by both.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

SERVER=${SSH_INTEROP_SERVER:-build/ssh-interop/server}
for command in ssh ssh-keygen timeout; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done
[[ -x $SERVER ]] || {
    echo "SSH interop server is missing: $SERVER" >&2
    exit 1
}

WORK=$(mktemp -d)
SERVER_PID=
cleanup() {
    local status=$?
    if [[ -n $SERVER_PID ]]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
    exit "$status"
}
trap cleanup EXIT

PORT=$(python3 - <<'PY'
import socket
with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
    sock.bind(("127.0.0.1", 0))
    print(sock.getsockname()[1])
PY
)

ssh-keygen -q -t ecdsa -b 256 -N '' -f "$WORK/client"
ssh-keygen -q -t ecdsa -b 256 -N '' -f "$WORK/other"
cp "$WORK/client.pub" "$WORK/authorized.pub"

start_server() {
    local tag=$1
    local scalar=$2
    local host_out=$3
    local fragment=${4:-}
    local -a extra=()
    if [[ -n $fragment ]]; then
        extra+=(--fragment)
    fi
    "$SERVER" --port "$PORT" --authorized-key "$WORK/authorized.pub" \
        --host-key-out "$host_out" --scalar "$scalar" "${extra[@]}" \
        >"$WORK/server-$tag.out" 2>"$WORK/server-$tag.err" &
    SERVER_PID=$!
    for _ in $(seq 1 100); do
        [[ -s $host_out ]] && break
        sleep 0.05
    done
    [[ -s $host_out ]] || {
        echo "server did not publish its host key ($tag)" >&2
        return 1
    }
    printf '[127.0.0.1]:%s ecdsa-sha2-nistp256 %s\n' "$PORT" \
        "$(cat "$host_out")" >"$WORK/known_hosts"
}

run_ssh() {
    local key=$1
    local command=$2
    shift 2
    timeout 20 ssh -F /dev/null -p "$PORT" -i "$key" \
        -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$WORK/known_hosts" \
        -o StrictHostKeyChecking=yes \
        -o BatchMode=yes \
        -o KexAlgorithms=ecdh-sha2-nistp256 \
        -o Ciphers=aes128-gcm@openssh.com \
        -o MACs=hmac-sha2-256 \
        "$@" hfoc@127.0.0.1 "$command"
}

run_ssh_aes256() {
    local command=$1
    shift
    timeout 20 ssh -F /dev/null -p "$PORT" -i "$WORK/client" \
        -o IdentitiesOnly=yes \
        -o UserKnownHostsFile="$WORK/known_hosts" \
        -o StrictHostKeyChecking=yes \
        -o BatchMode=yes \
        -o KexAlgorithms=ecdh-sha2-nistp256 \
        -o Ciphers=aes256-gcm@openssh.com \
        -o MACs=hmac-sha2-256 \
        "$@" hfoc@127.0.0.1 "$command"
}

stop_server() {
    if [[ -n $SERVER_PID ]]; then
        wait "$SERVER_PID" 2>/dev/null || true
        SERVER_PID=
    fi
}

# 1. The publickey handshake and the one useful command.
start_server first "$WORK/host.scalar" "$WORK/host-first.b64"
set +e
run_ssh "$WORK/client" 'hfoc status' >"$WORK/status.out" 2>"$WORK/status.err"
ssh_status=$?
set -e
stop_server
printf 'ssh_exit=%s\n' "$ssh_status"
cat "$WORK/status.out"
if [[ $ssh_status -ne 0 ]]; then
    cat "$WORK/status.err" >&2
    echo "stock OpenSSH did not complete the handshake" >&2
    exit 1
fi
grep -Fq 'HFOC-SPIKE status' "$WORK/status.out" || {
    echo "missing status body" >&2
    exit 1
}
grep -Fq 'kex=ecdh-sha2-nistp256 hostkey=ecdsa-sha2-nistp256' \
    "$WORK/status.out" || {
    echo "status body did not name the negotiated algorithms" >&2
    exit 1
}
grep -Fq 'auth=publickey' "$WORK/status.out" || {
    echo "missing publickey auth proof" >&2
    exit 1
}

# 1b. The operator collector pins known_hosts, keeps the key in a file and
#     verifies the diag BEGIN/END count and CRC before naming a report valid.
start_server collector "$WORK/host.scalar" "$WORK/host-collector.b64"
collected=$(
    "$ROOT/scripts/hfoc-ssh-collect.sh" \
        --host 127.0.0.1 --port "$PORT" \
        --key "$WORK/client" --known-hosts "$WORK/known_hosts" \
        --out "$WORK/collected" --command diag
)
stop_server
[[ -s $collected ]] || {
    echo "the collector did not write a diagnostic file" >&2
    exit 1
}
grep -Fq 'verified=yes' "${collected%.txt}.status" || {
    echo "the collector did not verify the diag count/CRC" >&2
    exit 1
}
grep -Fq 'records=2' "${collected%.txt}.status" || {
    echo "the collector did not record the diag record count" >&2
    exit 1
}
grep -Fq 'crc=5564A3C0' "${collected%.txt}.status" || {
    echo "the collector recorded the wrong diag CRC" >&2
    exit 1
}


# 1c. The same real OpenSSH handshake and diag stream, fed one byte at a time:
#     a split KEXINIT/packet must not change the result.
start_server fragment "$WORK/host.scalar" "$WORK/host-fragment.b64" fragment
set +e
run_ssh "$WORK/client" 'hfoc diag' >"$WORK/fragment.out" 2>"$WORK/fragment.err"
fragment_exit=$?
set -e
stop_server
(( fragment_exit == 0 )) || {
    cat "$WORK/fragment.err" >&2
    echo "the fragmented OpenSSH diag stream failed" >&2
    exit 1
}
grep -Fq 'mode=STREAM' "$WORK/fragment.out" || {
    echo "the fragmented diag stream lost its mode marker" >&2
    exit 1
}
grep -Fq 'state=END' "$WORK/fragment.out" || {
    echo "the fragmented diag stream lost its END record" >&2
    exit 1
}
grep -Fq 'records=2' "$WORK/fragment.out" || {
    echo "the fragmented diag stream lost its record count" >&2
    exit 1
}
grep -Fq 'crc=5564A3C0' "$WORK/fragment.out" || {
    echo "the fragmented diag stream lost its CRC" >&2
    exit 1
}

# 1d. AES-256-GCM must derive a 32-byte key and complete the real handshake.
start_server aes256 "$WORK/host.scalar" "$WORK/host-aes256.b64"
set +e
run_ssh_aes256 'hfoc status' >"$WORK/aes256.out" 2>"$WORK/aes256.err"
aes256_exit=$?
set -e
stop_server
(( aes256_exit == 0 )) || {
    cat "$WORK/aes256.err" >&2
    echo "the AES-256-GCM OpenSSH session failed" >&2
    exit 1
}
grep -Fq 'HFOC-SPIKE status' "$WORK/aes256.out" || {
    echo "the AES-256-GCM session lost the command body" >&2
    exit 1
}

# 1e. ITEM-616: the stock OpenSSH client's default MAC name-lists make the
#     KEXINIT transcript larger than 2048 bytes.  Leaving MACs at the client
#     default (the only variable from 1d) must still complete the handshake;
#     the transport hashes the transcript incrementally, not into a fixed
#     2048-byte buffer.
start_server defaultmacs "$WORK/host.scalar" "$WORK/host-defaultmacs.b64"
set +e
timeout 20 ssh -F /dev/null -p "$PORT" -i "$WORK/client" \
    -o IdentitiesOnly=yes \
    -o UserKnownHostsFile="$WORK/known_hosts" \
    -o StrictHostKeyChecking=yes \
    -o BatchMode=yes \
    hfoc@127.0.0.1 'hfoc status' \
    >"$WORK/defaultmacs.out" 2>"$WORK/defaultmacs.err"
defaultmacs_exit=$?
set -e
stop_server
(( defaultmacs_exit == 0 )) || {
    cat "$WORK/defaultmacs.err" >&2
    echo "the standard OpenSSH default MAC name-list was rejected" >&2
    exit 1
}
grep -Fq 'HFOC-SPIKE status' "$WORK/defaultmacs.out" || {
    echo "the default-MAC session lost the command body" >&2
    exit 1
}

# 2. The same scalar must publish the same host identity after a restart.
start_server second "$WORK/host.scalar" "$WORK/host-second.b64"
stop_server
cmp -s "$WORK/host-first.b64" "$WORK/host-second.b64" || {
    echo "the persisted host key changed between runs" >&2
    exit 1
}

# 3. An unauthorized client key is refused and gets no command output.
start_server third "$WORK/host.scalar" "$WORK/host-third.b64"
set +e
run_ssh "$WORK/other" 'hfoc status' >"$WORK/other.out" 2>"$WORK/other.err"
other_status=$?
set -e
stop_server
if [[ $other_status -eq 0 ]]; then
    echo "an unauthorized client key was accepted" >&2
    exit 1
fi
if grep -Fq 'HFOC-SPIKE status' "$WORK/other.out"; then
    echo "an unauthorized client key received command output" >&2
    exit 1
fi

# 4. A command outside the allowlist is refused with a nonzero status and no
#    shell execution.
start_server fourth "$WORK/host.scalar" "$WORK/host-fourth.b64"
set +e
run_ssh "$WORK/client" 'echo pwned' >"$WORK/refused.out" 2>"$WORK/refused.err"
refused_status=$?
set -e
stop_server
if [[ $refused_status -eq 0 ]]; then
    echo "an unknown command was accepted" >&2
    exit 1
fi
if grep -Fq 'pwned' "$WORK/refused.out"; then
    echo "a refused command was executed" >&2
    exit 1
fi
grep -Fq 'refused' "$WORK/refused.out" || {
    echo "the refusal body was not published" >&2
    exit 1
}

echo "SSH interop host proof: ALL OK"
