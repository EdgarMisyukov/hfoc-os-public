#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

verify() {
    local third_party=$1
    bash "$ROOT/scripts/verify-lwip-lock.sh" "$third_party"
}

expect_fail() {
    local label=$1
    shift
    if "$@" >/dev/null 2>&1; then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

make_fixture() {
    local target=$1
    cp -a "$ROOT/third_party" "$target"
}

lwip_file_count() {
    sed -n '/^lwip_lock_version=1$/,$p' "$1" |
        sed -n 's/^file_count=//p' |
        head -n 1
}

update_lwip_count() {
    local lock=$1
    local count=$2
    awk -v count="$count" '
        /^lwip_lock_version=1$/{p=1}
        p && /^file_count=/{print "file_count=" count; p=0; next}
        {print}
    ' "$lock" >"$lock.tmp"
    mv "$lock.tmp" "$lock"
}

verify "$ROOT/third_party"

bash "$ROOT/scripts/generate-lwip-lock.sh" \
    "$ROOT/third_party" "$TEST_DIR/generated.lock"
cmp "$ROOT/third_party/LOCKS.sha256" "$TEST_DIR/generated.lock"

make_fixture "$TEST_DIR/altered"
printf 'tampered\n' >>"$TEST_DIR/altered/lwip/src/core/def.c"
expect_fail 'altered lwIP source' verify "$TEST_DIR/altered"

make_fixture "$TEST_DIR/missing"
rm "$TEST_DIR/missing/lwip/src/core/def.c"
expect_fail 'missing lwIP source' verify "$TEST_DIR/missing"

make_fixture "$TEST_DIR/extra"
printf '// extra input\n' >"$TEST_DIR/extra/lwip/src/core/extra.c"
expect_fail 'extra lwIP source' verify "$TEST_DIR/extra"

make_fixture "$TEST_DIR/unlicensed"
UNLICENSED_FILE="$TEST_DIR/unlicensed/lwip/src/core/unlicensed.c"
printf '// unlicensed input\n' >"$UNLICENSED_FILE"
UNLICENSED_HASH=$(sha256sum "$UNLICENSED_FILE" | awk '{print $1}')
UNLICENSED_COUNT=$(lwip_file_count "$TEST_DIR/unlicensed/LOCKS.sha256")
update_lwip_count "$TEST_DIR/unlicensed/LOCKS.sha256" \
    "$((UNLICENSED_COUNT + 1))"
printf '%s  lwip/src/core/unlicensed.c  UNLICENSED\n' \
    "$UNLICENSED_HASH" >>"$TEST_DIR/unlicensed/LOCKS.sha256"
expect_fail 'unlicensed lwIP source' verify "$TEST_DIR/unlicensed"

make_fixture "$TEST_DIR/no-lock"
rm "$TEST_DIR/no-lock/LOCKS.sha256"
expect_fail 'missing source lock' verify "$TEST_DIR/no-lock"

make_fixture "$TEST_DIR/no-licenses"
rm "$TEST_DIR/no-licenses/LICENSES.md"
expect_fail 'missing license inventory' verify "$TEST_DIR/no-licenses"

make_fixture "$TEST_DIR/escape"
printf '%s  lwip/../outside.c  BSD-3-Clause\n' \
    "$(printf '0000000000000000000000000000000000000000000000000000000000000000')" \
    >>"$TEST_DIR/escape/LOCKS.sha256"
expect_fail 'path escape in lock' verify "$TEST_DIR/escape"

make_fixture "$TEST_DIR/duplicate"
FIRST_HASH=$(sed -n '/^files_begin$/,$p' \
    "$TEST_DIR/duplicate/LOCKS.sha256" | sed -n '2p' | awk '{print $1}')
FIRST_PATH=$(sed -n '/^files_begin$/,$p' \
    "$TEST_DIR/duplicate/LOCKS.sha256" | sed -n '2p' | awk '{print $2}')
printf '%s  %s  BSD-3-Clause\n' "$FIRST_HASH" "$FIRST_PATH" \
    >>"$TEST_DIR/duplicate/LOCKS.sha256"
expect_fail 'duplicate lock entry' verify "$TEST_DIR/duplicate"

make_fixture "$TEST_DIR/invalid-digest"
awk '
    /^lwip_lock_version=1$/{p=1}
    p && /^[0-9a-f]{64}  / && !done { sub(/^[0-9a-f]{64}/, "bad"); done=1 }
    {print}
' "$TEST_DIR/invalid-digest/LOCKS.sha256" >"$TEST_DIR/invalid-digest/lock.tmp"
mv "$TEST_DIR/invalid-digest/lock.tmp" \
    "$TEST_DIR/invalid-digest/LOCKS.sha256"
expect_fail 'invalid lock digest' verify "$TEST_DIR/invalid-digest"

make_fixture "$TEST_DIR/symlink"
ln -s core/def.c "$TEST_DIR/symlink/lwip/src/core/link.c"
SYMLINK_HASH=$(sha256sum "$TEST_DIR/symlink/lwip/src/core/def.c" | \
    awk '{print $1}')
SYMLINK_COUNT=$(lwip_file_count "$TEST_DIR/symlink/LOCKS.sha256")
update_lwip_count "$TEST_DIR/symlink/LOCKS.sha256" \
    "$((SYMLINK_COUNT + 1))"
printf '%s  lwip/src/core/link.c  BSD-3-Clause\n' \
    "$SYMLINK_HASH" >>"$TEST_DIR/symlink/LOCKS.sha256"
expect_fail 'symlinked lock input' verify "$TEST_DIR/symlink"

echo 'lwIP source and license lock host test: OK'
