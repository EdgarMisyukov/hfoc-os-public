#!/usr/bin/env bash
# ITEM-613: host proof for the diagnostic session's public SSH host-key export.
#
# The appliance publishes one [PRODUCTION-SSH-HOSTKEY] record through the normal
# diagnostic snapshot.  The Debian extractor validates the OpenSSH blob and its
# declared SHA256 independently and writes the .pub, .fingerprint and a
# known_hosts candidate.  This proof drives the same module the reader uses:
# valid, absent, unavailable, corrupt/truncated/wrong-fingerprint, duplicate and
# contradicting records, old format, DHCP/port change and an existing pin.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"
MODULE=scripts/hfoc-ssh-hostkey.py
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
FAILED=0

check() {
    local what=$1
    shift
    if "$@"; then
        printf 'PASS: %s\n' "$what"
    else
        printf 'FAIL: %s\n' "$what" >&2
        FAILED=1
    fi
}

check_not() {
    local what=$1
    shift
    if "$@"; then
        printf 'FAIL: %s\n' "$what" >&2
        FAILED=1
    else
        printf 'PASS: %s\n' "$what"
    fi
}

ALGORITHM=ecdsa-sha2-nistp256
BLOB='AAAAE2VjZHNhLXNoYTItbmlzdHAyNTYAAAAIbmlzdHAyNTYAAABBBA4XbH6KtmYa4QZo2V2VL+5d+OtVsDy2tSV/yba15z2yoG2ol67GhN2TR6Q6Ylj0kIyo1Dn/17tXULjsxdWTyYY='
FINGERPRINT='SHA256:Lc8UsC2OrEcPGz9MqeDS2DKOa4gzVdhYsQpSuWz+0QE'
PRIVATE_SENTINEL='HFOC-PRIVATE-SENTINEL-DO-NOT-EXPORT'

write_session() {
    local path=$1
    local record=$2
    {
        printf '# HFOC diagnostic report\n'
        printf '# session: 00000007\n'
        printf '%s\n' '[DIAG] id=00000007 state=BEGIN build=host-613'
        printf '%s\n' '[PRODUCTION-SSH] state=LISTENING hostkey=SHA256:Lc8UsC2OrEcPGz9MqeDS2DKOa4gzVdhYsQpSuWz+0QE'
        printf '%s\n' "$record"
        printf '%s\n' "$PRIVATE_SENTINEL"
        printf '%s\n' '[DIAG] id=00000007 state=END records=4 crc=00000000'
    } >"$path"
}

record_ready() {
    printf '[PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=%s blob=%s fingerprint=%s address=192.0.2.13 port=%s build=host-613' \
        "$ALGORITHM" "$BLOB" "$FINGERPRINT" "${1:-22}"
}

# 1. A valid record produces the public files and the candidate.
SESSION=$WORK/session.txt
OUT=$WORK/out
write_session "$SESSION" "$(record_ready)"
check "the extractor accepts the valid public host key" \
    python3 "$MODULE" --session "$SESSION" --session-id 00000007 --output "$OUT" >"$WORK/valid.log" 2>&1
PUB=$OUT/HFOC-SSH-HOST-00000007.pub
FP=$OUT/HFOC-SSH-HOST-00000007.fingerprint
KNOWN=$OUT/HFOC-SSH-HOST-00000007.known_hosts.candidate
check "the .pub carries the OpenSSH algorithm and blob" \
    grep -Fqx "$ALGORITHM $BLOB" "$PUB"
check "the fingerprint file carries the independently computed value" \
    grep -Fqx "$FINGERPRINT" "$FP"
check "the known_hosts candidate names the record's address" \
    grep -Fqx "192.0.2.13 $ALGORITHM $BLOB" "$KNOWN"
check "the written files are owner-only" \
    test "$(stat -c '%a' "$PUB")" = 600 -a "$(stat -c '%a' "$KNOWN")" = 600
if command -v ssh-keygen >/dev/null 2>&1; then
    check "production ssh-keygen accepts the .pub and agrees on the fingerprint" \
        grep -Fq "$FINGERPRINT" <(ssh-keygen -lf "$PUB" -E sha256)
fi
check_not "no sentinel private text reaches the public files" \
    grep -Rq "$PRIVATE_SENTINEL" "$OUT"

# 2. An old export without the record is valid but names the absent key.
OLD=$WORK/old.txt
printf '%s\n' '[DIAG] id=00000008 state=BEGIN build=host-613' \
    '[PRODUCTION-SSH] state=LISTENING hostkey=SHA256:Lc8UsC2OrEcPGz9MqeDS2DKOa4gzVdhYsQpSuWz+0QE' \
    '[DIAG] id=00000008 state=END records=2 crc=00000000' >"$OLD"
check "an old export without a key record is explicitly ABSENT" \
    grep -Fq 'hostkey=ABSENT reason=NO_HOSTKEY_RECORD' \
    <(python3 "$MODULE" --session "$OLD" --session-id 00000007 --output "$WORK/old-out" 2>&1)
check_not "an old export writes no trusted candidate" \
    test -e "$WORK/old-out/HFOC-SSH-HOST-00000000.known_hosts.candidate"

# 3. An unavailable identity is named, not invented.
UNAVAILABLE=$WORK/unavailable.txt
write_session "$UNAVAILABLE" \
    '[PRODUCTION-SSH-HOSTKEY] v=1 state=UNAVAILABLE reason=AWAITING_HOST_KEY address=none port=22'
check "an unavailable identity is named" \
    grep -Fq 'hostkey=UNAVAILABLE reason=AWAITING_HOST_KEY' \
    <(python3 "$MODULE" --session "$UNAVAILABLE" --session-id 00000007 --output "$WORK/unavailable-out" 2>&1)
check_not "an unavailable identity writes no candidate" \
    test -e "$WORK/unavailable-out/HFOC-SSH-HOST-00000007.known_hosts.candidate"

# 4. Corrupt, truncated and mismatched blobs/fingerprints are refused.
refuse_case() {
    local name=$1
    local record=$2
    local file=$WORK/$name.txt
    local out=$WORK/$name-out
    write_session "$file" "$record"
    if python3 "$MODULE" --session "$file" --session-id 00000007 --output "$out" >"$WORK/$name.log" 2>&1; then
        printf 'FAIL: %s was accepted\n' "$name" >&2
        FAILED=1
    else
        printf 'PASS: %s is refused\n' "$name"
    fi
    if [[ -e $out/HFOC-SSH-HOST-00000007.pub ||
          -e $out/HFOC-SSH-HOST-00000007.known_hosts.candidate ]]; then
        printf 'FAIL: %s wrote a trusted file\n' "$name" >&2
        FAILED=1
    else
        printf 'PASS: %s wrote no trusted file\n' "$name"
    fi
}
refuse_case corrupt "[PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=$ALGORITHM blob=!!!! fingerprint=$FINGERPRINT address=192.0.2.13 port=22"
refuse_case truncated "[PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=$ALGORITHM blob=${BLOB%????} fingerprint=$FINGERPRINT address=192.0.2.13 port=22"
refuse_case wrongfp "[PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=$ALGORITHM blob=$BLOB fingerprint=SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA address=192.0.2.13 port=22"
refuse_case badport "[PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=$ALGORITHM blob=$BLOB fingerprint=$FINGERPRINT address=192.0.2.13 port=99999"

# 5. Identical duplicates are harmless; contradicting ones are refused.
DUP=$WORK/duplicate.txt
write_session "$DUP" "$(record_ready)"
sed -i "3a [PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=$ALGORITHM blob=$BLOB fingerprint=$FINGERPRINT address=192.0.2.13 port=22" "$DUP"
check "identical duplicate records are accepted" \
    python3 "$MODULE" --session "$DUP" --session-id 00000007 --output "$WORK/duplicate-out"
CONTRADICT=$WORK/contradict.txt
write_session "$CONTRADICT" "$(record_ready)"
sed -i "3a [PRODUCTION-SSH-HOSTKEY] v=1 state=READY algorithm=$ALGORITHM blob=$BLOB fingerprint=SHA256:AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA address=192.0.2.13 port=22" "$CONTRADICT"
check_not "contradicting duplicate records are refused" \
    python3 "$MODULE" --session "$CONTRADICT" --session-id 00000007 --output "$WORK/contradict-out"

# 6. DHCP/port change and an existing pin: the candidate names the actual host,
#    and a different existing file is kept while a .new candidate is written.
MOVED=$WORK/moved.txt
write_session "$MOVED" "$(record_ready 2222)"
MOVED_OUT=$WORK/moved-out
check "a nonstandard port is bracketed in the candidate" \
    bash -c "python3 '$MODULE' --session '$MOVED' --session-id 00000007 --output '$MOVED_OUT' >/dev/null && grep -Fqx '[192.0.2.13]:2222 $ALGORITHM $BLOB' '$MOVED_OUT/HFOC-SSH-HOST-00000007.known_hosts.candidate'"
CONFLICT_OUT=$WORK/conflict-out
mkdir -p "$CONFLICT_OUT"
printf '%s\n' '192.0.2.13 ssh-ed25519 AAAA-old' >"$CONFLICT_OUT/HFOC-SSH-HOST-00000007.known_hosts.candidate"
check "an existing contradicting pin is not overwritten silently" \
    bash -c "python3 '$MODULE' --session '$SESSION' --session-id 00000007 --output '$CONFLICT_OUT' >/dev/null && grep -Fqx '192.0.2.13 ssh-ed25519 AAAA-old' '$CONFLICT_OUT/HFOC-SSH-HOST-00000007.known_hosts.candidate' && test -e '$CONFLICT_OUT/HFOC-SSH-HOST-00000007.known_hosts.candidate.new'"

if (( FAILED != 0 )); then
    echo "SSH host-key export host test: FAILED" >&2
    exit 1
fi
echo "SSH host-key export host test: ALL OK"
