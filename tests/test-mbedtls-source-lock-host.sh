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
    bash "$ROOT/scripts/verify-mbedtls-lock.sh" "$third_party"
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

insert_before_lwip() {
    local lock=$1
    local line=$2
    awk -v line="$line" \
        '/^lwip_lock_version=1$/{print line} {print}' \
        "$lock" >"$lock.tmp"
    mv "$lock.tmp" "$lock"
}

verify "$ROOT/third_party"

bash "$ROOT/scripts/generate-mbedtls-lock.sh" \
    "$ROOT/third_party" "$TEST_DIR/generated.lock"
cmp "$ROOT/third_party/LOCKS.sha256" "$TEST_DIR/generated.lock"

make_fixture "$TEST_DIR/altered"
printf 'tampered\n' >>"$TEST_DIR/altered/mbedtls/library/aes.c"
expect_fail 'altered mbedTLS source' verify "$TEST_DIR/altered"

make_fixture "$TEST_DIR/missing"
rm "$TEST_DIR/missing/mbedtls/library/aes.c"
expect_fail 'missing mbedTLS source' verify "$TEST_DIR/missing"

make_fixture "$TEST_DIR/extra"
printf '// extra input\n' >"$TEST_DIR/extra/mbedtls/library/extra.c"
expect_fail 'extra mbedTLS source' verify "$TEST_DIR/extra"

make_fixture "$TEST_DIR/unlicensed"
UNLICENSED_FILE="$TEST_DIR/unlicensed/mbedtls/library/unlicensed.c"
printf '// unlicensed input\n' >"$UNLICENSED_FILE"
UNLICENSED_HASH=$(sha256sum "$UNLICENSED_FILE" | awk '{print $1}')
UNLICENSED_COUNT=$(sed -n 's/^file_count=//p' \
    "$TEST_DIR/unlicensed/LOCKS.sha256" | head -n 1)
sed -i "0,/^file_count=.*/s//file_count=$((UNLICENSED_COUNT + 1))/" \
    "$TEST_DIR/unlicensed/LOCKS.sha256"
insert_before_lwip "$TEST_DIR/unlicensed/LOCKS.sha256" \
    "$UNLICENSED_HASH  mbedtls/library/unlicensed.c  UNLICENSED"
expect_fail 'unlicensed mbedTLS source' verify "$TEST_DIR/unlicensed"

make_fixture "$TEST_DIR/no-lock"
rm "$TEST_DIR/no-lock/LOCKS.sha256"
expect_fail 'missing source lock' verify "$TEST_DIR/no-lock"

make_fixture "$TEST_DIR/no-licenses"
rm "$TEST_DIR/no-licenses/LICENSES.md"
expect_fail 'missing license inventory' verify "$TEST_DIR/no-licenses"

make_fixture "$TEST_DIR/no-license-text"
rm "$TEST_DIR/no-license-text/mbedtls/LICENSE"
expect_fail 'missing mbedTLS license text' verify "$TEST_DIR/no-license-text"

make_fixture "$TEST_DIR/escape"
insert_before_lwip "$TEST_DIR/escape/LOCKS.sha256" \
    '0000000000000000000000000000000000000000000000000000000000000000  mbedtls/../outside.c  Apache-2.0 OR GPL-2.0-or-later'
expect_fail 'path escape in lock' verify "$TEST_DIR/escape"

make_fixture "$TEST_DIR/duplicate"
FIRST_HASH=$(awk \
    '/^mbedtls_files_begin$/{p=1; next} p&&/^[0-9a-f]{64}  /{print $1; exit}' \
    "$TEST_DIR/duplicate/LOCKS.sha256")
FIRST_PATH=$(awk \
    '/^mbedtls_files_begin$/{p=1; next} p&&/^[0-9a-f]{64}  /{print $2; exit}' \
    "$TEST_DIR/duplicate/LOCKS.sha256")
DUPLICATE_COUNT=$(sed -n 's/^file_count=//p' \
    "$TEST_DIR/duplicate/LOCKS.sha256" | head -n 1)
sed -i "0,/^file_count=.*/s//file_count=$((DUPLICATE_COUNT + 1))/" \
    "$TEST_DIR/duplicate/LOCKS.sha256"
insert_before_lwip "$TEST_DIR/duplicate/LOCKS.sha256" \
    "$FIRST_HASH  $FIRST_PATH  Apache-2.0 OR GPL-2.0-or-later"
expect_fail 'duplicate lock entry' verify "$TEST_DIR/duplicate"

make_fixture "$TEST_DIR/invalid-digest"
sed -i '0,/^[0-9a-f]\{64\}  /s//bad  /' \
    "$TEST_DIR/invalid-digest/LOCKS.sha256"
expect_fail 'invalid lock digest' verify "$TEST_DIR/invalid-digest"

make_fixture "$TEST_DIR/symlink"
ln -s aes.c "$TEST_DIR/symlink/mbedtls/library/link.c"
SYMLINK_HASH=$(sha256sum "$TEST_DIR/symlink/mbedtls/library/aes.c" | \
    awk '{print $1}')
SYMLINK_COUNT=$(sed -n 's/^file_count=//p' \
    "$TEST_DIR/symlink/LOCKS.sha256" | head -n 1)
sed -i "0,/^file_count=.*/s//file_count=$((SYMLINK_COUNT + 1))/" \
    "$TEST_DIR/symlink/LOCKS.sha256"
insert_before_lwip "$TEST_DIR/symlink/LOCKS.sha256" \
    "$SYMLINK_HASH  mbedtls/library/link.c  Apache-2.0 OR GPL-2.0-or-later"
expect_fail 'symlinked lock input' verify "$TEST_DIR/symlink"

make_fixture "$TEST_DIR/metadata-drift"
sed -i 's/^upstream_version=mbedtls-3.6.7/upstream_version=mbedtls-3.6.6/' \
    "$TEST_DIR/metadata-drift/LOCKS.sha256"
expect_fail 'upstream release drift' verify "$TEST_DIR/metadata-drift"

echo 'mbedTLS source and license lock host test: OK'
