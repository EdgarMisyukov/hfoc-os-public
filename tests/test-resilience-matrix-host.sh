#!/usr/bin/env bash
# Host regression harness for the deterministic resilience matrix runner.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
FAKE_MAKE=$TEST_DIR/fake-make
FAKE_STATE=$TEST_DIR/state
ARTIFACT_DIR=$TEST_DIR/artifacts
export FAKE_STATE
mkdir -p "$FAKE_STATE"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if [[ -f "$FAKE_STATE/pids" ]]; then
        while read -r pid; do
            kill "$pid" 2>/dev/null || true
        done <"$FAKE_STATE/pids"
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cat >"$FAKE_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

state=$FAKE_STATE
[[ $# == 5 && $1 == '-o' && $2 == 'deps' && $3 == '-o' && $4 == 'iso' ]] || {
    echo "matrix must skip already-prepared deps and iso" >&2
    exit 64
}
target=$5
printf 'start %s\n' "$target" >>"$state/events"

case "$target" in
    fake-a|fake-b)
        printf '%s\n' "$$" >>"$state/pids"
        sleep 0.05
        printf 'end %s\n' "$target" >>"$state/events"
        ;;
    fake-fail)
        exit 23
        ;;
    *)
        printf 'unknown target: %s\n' "$target" >&2
        exit 64
        ;;
esac
FAKE
chmod +x "$FAKE_MAKE"

: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if ! matrix_output=$(
    HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_RESILIENCE_MATRIX_TARGETS="fake-a fake-b" \
    HFOC_RESILIENCE_MATRIX_ARTIFACT_DIR="$ARTIFACT_DIR" \
        bash scripts/test-resilience-matrix.sh
); then
    echo 'resilience matrix success case failed' >&2
    exit 1
fi

grep -Fxq 'start fake-a' "$FAKE_STATE/events"
grep -Fxq 'end fake-a' "$FAKE_STATE/events"
grep -Fxq 'start fake-b' "$FAKE_STATE/events"
grep -Fxq 'end fake-b' "$FAKE_STATE/events"
[[ "$(grep -c '^start ' "$FAKE_STATE/events")" == 2 ]] || {
    echo 'matrix did not invoke exactly two targets' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
[[ -f "$ARTIFACT_DIR/fake-a.log" && -f "$ARTIFACT_DIR/fake-b.log" ]] || {
    echo 'matrix did not persist per-target artifacts' >&2
    exit 1
}
grep -Fq 'QEMU suite: PASS fake-a' "$ARTIFACT_DIR/matrix-suite.log"
grep -Fq 'QEMU suite: PASS fake-b' "$ARTIFACT_DIR/matrix-suite.log"
grep -Fq 'PASS fake-a' "$ARTIFACT_DIR/matrix-manifest.txt"
grep -Fq 'PASS fake-b' "$ARTIFACT_DIR/matrix-manifest.txt"
grep -Fq 'Resilience matrix: OK' <<<"$matrix_output"

rm -rf "$ARTIFACT_DIR"
: >"$FAKE_STATE/events"
: >"$FAKE_STATE/pids"
if HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_RESILIENCE_MATRIX_TARGETS="fake-fail fake-b" \
    HFOC_RESILIENCE_MATRIX_ARTIFACT_DIR="$ARTIFACT_DIR" \
    bash scripts/test-resilience-matrix.sh >/dev/null 2>&1; then
    echo 'expected resilience matrix failure' >&2
    exit 1
fi
grep -Fxq 'start fake-fail' "$FAKE_STATE/events"
if grep -Fxq 'start fake-b' "$FAKE_STATE/events"; then
    echo 'matrix started a queued target after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
[[ -f "$ARTIFACT_DIR/matrix-manifest.txt" ]] || {
    echo 'matrix did not preserve manifest after failure' >&2
    exit 1
}
grep -Fq 'FAIL fake-fail' "$ARTIFACT_DIR/matrix-manifest.txt"

if pgrep -f "^$FAKE_MAKE" >/dev/null; then
    echo 'fake make child leaked after matrix failure' >&2
    pgrep -af "^$FAKE_MAKE" >&2 || true
    exit 1
fi

echo 'Resilience matrix host test: OK'
