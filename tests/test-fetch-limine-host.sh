#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TEST_DIR=$(mktemp -d)
REPO="$TEST_DIR/repo"
FIXTURE_ROOT="$TEST_DIR/fixture"
FAKE_BIN="$TEST_DIR/bin"
ARCHIVE="$TEST_DIR/limine-fixture.tar.gz"
CACHE_DIR="$TEST_DIR/persistent-cache"
CURL_LOG="$TEST_DIR/curl.log"
BUILD_LOG="$TEST_DIR/build.log"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$REPO/scripts" "$FIXTURE_ROOT/limine-fixture" "$FAKE_BIN"
cp "$ROOT/scripts/fetch-limine.sh" "$REPO/scripts/fetch-limine.sh"
chmod +x "$REPO/scripts/fetch-limine.sh"

# ITEM-517: the pinned 12.3.2 release also ships the stage1/stage2 blob header
# (`limine-bios-hdd.h`), and the installer kernel transport requires it, so the
# fixture tree carries it like the other five files.
for file in limine-bios.sys limine-bios-cd.bin limine-uefi-cd.bin BOOTX64.EFI \
    limine-bios-hdd.h; do
    printf 'fixture %s\n' "$file" >"$FIXTURE_ROOT/limine-fixture/$file"
done
cat >"$FIXTURE_ROOT/limine-fixture/Makefile" <<'EOF'
all:
	@printf 'build\n' >>"$$HFOC_TEST_BUILD_LOG"
	@touch limine
EOF
tar -czf "$ARCHIVE" -C "$FIXTURE_ROOT" limine-fixture
EXPECTED_SHA256=$(sha256sum "$ARCHIVE" | awk '{print $1}')

cat >"$FAKE_BIN/curl" <<'EOF'
#!/usr/bin/env bash
set -Eeuo pipefail

printf '%s\n' "$*" >>"$HFOC_TEST_CURL_LOG"
output=
while (($#)); do
    case "$1" in
        --output)
            output=${2:?missing curl output path}
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done
[[ -n "$output" ]]
cp "$HFOC_TEST_CURL_SOURCE" "$output"
EOF
chmod +x "$FAKE_BIN/curl"

run_fetch() {
    PATH="$FAKE_BIN:$PATH" \
    HFOC_LIMINE_VERSION=fixture \
    HFOC_LIMINE_EXPECTED_SHA256="$EXPECTED_SHA256" \
    HFOC_LIMINE_URL=https://example.invalid/limine-fixture.tar.gz \
    HFOC_LIMINE_CACHE_DIR="$CACHE_DIR" \
    HFOC_LIMINE_DEPS_DIR="$REPO/deps" \
    HFOC_TEST_CURL_SOURCE="$ARCHIVE" \
    HFOC_TEST_CURL_LOG="$CURL_LOG" \
    HFOC_TEST_BUILD_LOG="$BUILD_LOG" \
        bash "$REPO/scripts/fetch-limine.sh"
}

run_fetch

CACHE_ARCHIVE="$CACHE_DIR/limine-binary-fixture.tar.gz"
[[ -f "$CACHE_ARCHIVE" ]] || {
    echo 'Limine archive was not stored in the persistent cache' >&2
    exit 1
}
grep -Fq -- '--retry 4' "$CURL_LOG" || {
    echo 'Limine download did not configure bounded curl retries' >&2
    exit 1
}
grep -Fq -- '--retry-all-errors' "$CURL_LOG" || {
    echo 'Limine download did not retry transient TLS failures' >&2
    exit 1
}
[[ $(wc -l <"$CURL_LOG") -eq 1 ]]
[[ $(wc -l <"$BUILD_LOG") -eq 1 ]]

run_fetch
[[ $(wc -l <"$CURL_LOG") -eq 1 ]] || {
    echo 'Warm Limine target unexpectedly downloaded the archive again' >&2
    exit 1
}
[[ $(wc -l <"$BUILD_LOG") -eq 1 ]] || {
    echo 'Warm Limine target unexpectedly rebuilt the dependency' >&2
    exit 1
}

rm -rf "$REPO/deps"
run_fetch
[[ $(wc -l <"$CURL_LOG") -eq 1 ]] || {
    echo 'Clean checkout did not reuse the persistent Limine archive' >&2
    exit 1
}
[[ $(wc -l <"$BUILD_LOG") -eq 2 ]] || {
    echo 'Clean checkout did not rebuild Limine from the persistent archive' >&2
    exit 1
}

printf 'corrupt cache\n' >"$CACHE_ARCHIVE"
rm -rf "$REPO/deps"
run_fetch
[[ $(wc -l <"$CURL_LOG") -eq 2 ]] || {
    echo 'Corrupt Limine cache was not replaced by one fresh download' >&2
    exit 1
}
[[ $(wc -l <"$BUILD_LOG") -eq 3 ]] || {
    echo 'Recovered Limine archive was not prepared' >&2
    exit 1
}

echo 'Limine dependency cache host test: OK'
