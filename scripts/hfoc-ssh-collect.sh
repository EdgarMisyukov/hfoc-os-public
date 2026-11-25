#!/usr/bin/env bash
# ITEM-602: carry one HFOC diagnostic command off the machine over the ordinary
# SSH client.
#
# The collector is deliberately small and fail-closed:
#
#   * the private key is named as a file, never passed in argv, printed or
#     copied; the host key is pinned by a known_hosts file the operator filled
#     from the local console fingerprint;
#   * stdout, exit status and the diag session's build/session/count/CRC are
#     written into a private directory (0700, files 0600);
#   * a nonzero SSH exit, an empty body, a missing or duplicated diag END, a
#     record-count mismatch, a CRC mismatch or a `SAVED` claim all fail - a
#     partial stream is kept for diagnosis but never named as a valid report.
#
# Usage:
#   scripts/hfoc-ssh-collect.sh --host 192.0.2.13 \
#       --key ~/.ssh/hfoc_diag --known-hosts ~/.ssh/hfoc_known_hosts \
#       --out ~/hfoc-diag [--port 22] [--user hfoc] [--command diag]
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

HOST=
PORT=22
USER_NAME=hfoc
KEY=
KNOWN_HOSTS=
OUT=
COMMAND=diag

usage() {
    cat <<'EOF'
usage: hfoc-ssh-collect.sh --host ADDR --key FILE --known-hosts FILE --out DIR
                           [--port N] [--user NAME]
                           [--command status|network|journal|diag]
EOF
}

fail() {
    printf 'hfoc-ssh-collect: %s\n' "$*" >&2
    exit 1
}

while (($# > 0)); do
    case $1 in
    --host) HOST=${2:-}; shift 2 ;;
    --port) PORT=${2:-}; shift 2 ;;
    --user) USER_NAME=${2:-}; shift 2 ;;
    --key) KEY=${2:-}; shift 2 ;;
    --known-hosts) KNOWN_HOSTS=${2:-}; shift 2 ;;
    --out) OUT=${2:-}; shift 2 ;;
    --command) COMMAND=${2:-}; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) usage >&2; fail "unknown argument: $1" ;;
    esac
done

[[ -n $HOST ]] || fail "--host is required"
[[ -n $KEY ]] || fail "--key is required"
[[ -n $KNOWN_HOSTS ]] || fail "--known-hosts is required"
[[ -n $OUT ]] || fail "--out is required"
[[ $HOST =~ ^[A-Za-z0-9._:-]+$ ]] || fail "--host is not a plain address"
[[ $USER_NAME =~ ^[A-Za-z0-9._-]+$ ]] || fail "--user is not a plain name"
[[ $PORT =~ ^[0-9]+$ ]] || fail "--port is not a number"
case $COMMAND in
status|network|journal|diag) ;;
*) fail "--command must be status, network, journal or diag" ;;
esac
[[ -r $KEY ]] || fail "the private key file is not readable: $KEY"
[[ -s $KNOWN_HOSTS ]] || fail "the pinned known_hosts file is missing or empty"
command -v ssh >/dev/null || fail "ssh is not installed"
command -v python3 >/dev/null || fail "python3 is not installed"

umask 077
if [[ -e $OUT && ! -d $OUT ]]; then
    fail "--out exists and is not a directory"
fi
mkdir -p -- "$OUT"
chmod 700 -- "$OUT"

TMP=$(mktemp "$OUT/.hfoc-ssh-XXXXXX")
TMP_ERR=$(mktemp "$OUT/.hfoc-ssh-err-XXXXXX")
chmod 600 -- "$TMP" "$TMP_ERR"

cleanup() {
    local status=$?
    rm -f -- "$TMP" "$TMP_ERR"
    exit "$status"
}
trap cleanup EXIT

SSH_STATUS=0
set +e
ssh -F /dev/null -p "$PORT" -i "$KEY" \
    -o IdentitiesOnly=yes \
    -o BatchMode=yes \
    -o ConnectTimeout=15 \
    -o UserKnownHostsFile="$KNOWN_HOSTS" \
    -o StrictHostKeyChecking=yes \
    -o GlobalKnownHostsFile=/dev/null \
    -o KexAlgorithms=ecdh-sha2-nistp256 \
    -o Ciphers=aes128-gcm@openssh.com \
    -o MACs=hmac-sha2-256 \
    -o LogLevel=ERROR \
    "$USER_NAME@$HOST" "hfoc $COMMAND" >"$TMP" 2>"$TMP_ERR"
SSH_STATUS=$?
set -e

STAMP=$(date -u +%Y%m%dT%H%M%SZ)
BASE=$OUT/hfoc-$COMMAND-$STAMP
if ((SSH_STATUS != 0)); then
    mv -- "$TMP" "$BASE.failed"
    mv -- "$TMP_ERR" "$BASE.failed.stderr"
    chmod 600 -- "$BASE.failed" "$BASE.failed.stderr"
    printf 'ssh_exit=%s verified=no reason=ssh-status\n' "$SSH_STATUS" \
        >"$BASE.failed.status"
    fail "ssh exited $SSH_STATUS; partial output kept at $BASE.failed"
fi
if [[ ! -s $TMP ]]; then
    mv -- "$TMP" "$BASE.failed"
    mv -- "$TMP_ERR" "$BASE.failed.stderr"
    chmod 600 -- "$BASE.failed" "$BASE.failed.stderr"
    fail "ssh returned an empty body"
fi

if [[ $COMMAND == diag ]]; then
    VERIFY=$(
        python3 - "$TMP" <<'PY'
import re
import sys
import zlib

path = sys.argv[1]
begin = re.compile(r"^\[DIAG\] id=([0-9A-Fa-f]{8}) state=BEGIN(?: |$)")
end = re.compile(r"^\[DIAG\] id=([0-9A-Fa-f]{8}) state=END(?: |$)")
records_re = re.compile(r" records=(\d+)")
crc_re = re.compile(r" crc=([0-9A-Fa-f]{8})")
build_re = re.compile(r" build=(\S+)")

with open(path, "r", encoding="ascii", errors="strict") as handle:
    lines = handle.read().splitlines()

sessions = []
current = None
trailer = None
saved_claim = False
for line in lines:
    if re.search(r"\bSAVED\b", line):
        saved_claim = True
    match = begin.match(line)
    if match:
        if current is not None:
            raise SystemExit("diag: nested BEGIN")
        current = {"id": match.group(1).upper(), "records": [], "end": None,
                   "build": (build_re.search(line).group(1)
                             if build_re.search(line) else "unknown")}
        sessions.append(current)
        continue
    match = end.match(line)
    if match:
        if current is None:
            raise SystemExit("diag: END without BEGIN")
        if current["end"] is not None:
            raise SystemExit("diag: duplicated END")
        if current["id"] != match.group(1).upper():
            raise SystemExit("diag: END id does not match BEGIN")
        wanted = records_re.search(line)
        wanted_crc = crc_re.search(line)
        if wanted is None or wanted_crc is None:
            raise SystemExit("diag: END is malformed")
        current["end"] = (int(wanted.group(1)), int(wanted_crc.group(1), 16))
        continue
    if current is not None and current["end"] is None:
        if line.startswith("[DIAG] id="):
            raise SystemExit("diag: a second session marker is inside the body")
        current["records"].append(line)
        continue
    if line.startswith("[DIAG] transport=ssh"):
        if trailer is not None:
            raise SystemExit("diag: more than one stream trailer")
        trailer = line

if not sessions:
    raise SystemExit("diag: no BEGIN record")
if len(sessions) != 1:
    raise SystemExit("diag: more than one session")
session = sessions[0]
if session["end"] is None:
    raise SystemExit("diag: END is missing (truncated stream)")
count, wanted_crc = session["end"]
if count != len(session["records"]):
    raise SystemExit(
        f"diag: END names {count} records, stream carries {len(session['records'])}"
    )
computed = 0
for line in session["records"]:
    computed = zlib.crc32(line.encode("ascii") + b"\n", computed)
if computed != wanted_crc:
    raise SystemExit(
        f"diag: CRC {computed:08X} does not match END {wanted_crc:08X}"
    )
if saved_claim:
    raise SystemExit("diag: the stream claims a medium SAVED state")
if trailer is None:
    raise SystemExit("diag: the stream trailer is missing")
if "mode=STREAM" not in trailer or "saved=no" not in trailer:
    raise SystemExit("diag: the trailer is not a STREAM/not-SAVED record")
trailer_records = records_re.search(trailer)
trailer_crc = crc_re.search(trailer)
if trailer_records is None or trailer_crc is None:
    raise SystemExit("diag: the trailer has no records/CRC")
if int(trailer_records.group(1)) != count:
    raise SystemExit("diag: the trailer count does not match END")
if int(trailer_crc.group(1), 16) != computed:
    raise SystemExit("diag: the trailer CRC does not match END")
print(f"build={session['build']} session={session['id']} records={count} crc={computed:08X}")
PY
    ) || {
        mv -- "$TMP" "$BASE.failed"
        mv -- "$TMP_ERR" "$BASE.failed.stderr"
        chmod 600 -- "$BASE.failed" "$BASE.failed.stderr"
        printf 'ssh_exit=%s verified=no reason=diag-verify\n' "$SSH_STATUS" \
            >"$BASE.failed.status"
        fail "the diag stream did not verify; partial output kept at $BASE.failed"
    }
    mv -- "$TMP" "$BASE.txt"
    chmod 600 -- "$BASE.txt"
    printf 'ssh_exit=0 verified=yes %s\n' "$VERIFY" >"$BASE.status"
    chmod 600 -- "$BASE.status"
    rm -f -- "$TMP_ERR"
    printf '%s\n' "$BASE.txt"
else
    mv -- "$TMP" "$BASE.txt"
    chmod 600 -- "$BASE.txt"
    printf 'ssh_exit=0 verified=transport\n' >"$BASE.status"
    chmod 600 -- "$BASE.status"
    rm -f -- "$TMP_ERR"
    printf '%s\n' "$BASE.txt"
fi
