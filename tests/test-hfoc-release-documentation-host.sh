#!/usr/bin/env bash
# Host regression harness for the HFOC release documentation gate.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cp -a docs release "$TEST_DIR"/

run_verifier() {
    bash scripts/verify-hfoc-release-documentation.sh "$TEST_DIR"
}

expect_failure() {
    local label=$1
    local status=0
    shift

    set +e
    "$@" >/dev/null 2>&1
    status=$?
    set -e
    if (( status == 0 )); then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

if ! verifier_output=$(run_verifier); then
    echo 'canonical HFOC release documentation verification failed' >&2
    exit 1
fi
grep -Fq 'HFOC release documentation verification: OK' \
    <<<"$verifier_output"

expect_failure "missing installation guide" \
    bash scripts/verify-hfoc-release-documentation.sh "$TEST_DIR/missing"

rm "$TEST_DIR/docs/hfoc-installation-guide.md"
expect_failure "missing installation guide in copied docs" run_verifier

cp "$ROOT/docs/hfoc-installation-guide.md" "$TEST_DIR/docs/"
sed -i 's/## Safety Before Installation/## No Safety Section/' \
    "$TEST_DIR/docs/hfoc-installation-guide.md"
expect_failure "removed installation safety phrase" run_verifier

cp "$ROOT/docs/hfoc-installation-guide.md" "$TEST_DIR/docs/"
sed -i 's/| NOT TESTED |/| PASS |/' \
    "$TEST_DIR/docs/hfoc-supported-hardware-matrix.md"
expect_failure "hardware matrix records PASS" run_verifier

cp "$ROOT/release/hfoc-hardware-acceptance-report.md" \
    "$TEST_DIR/release/"
sed -i 's/| NOT TESTED |/| PASS |/' \
    "$TEST_DIR/release/hfoc-hardware-acceptance-report.md"
expect_failure "hardware report records PASS" run_verifier

if ! runner_output=$(bash scripts/test-hfoc-release-documentation.sh); then
    echo 'release documentation runner failed' >&2
    exit 1
fi
grep -Fq 'HFOC release documentation gate: OK' <<<"$runner_output"

echo 'HFOC release documentation host test: OK'
