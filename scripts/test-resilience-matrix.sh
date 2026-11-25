#!/usr/bin/env bash
# Run the deterministic Wave S resilience matrix and preserve artifacts.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in bash date grep mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

matrix_targets_default=(
    test-block-device
    test-storage-interruption-qemu
    test-service-cascade-drill-qemu
    test-local-control-outage-drill-qemu
    test-network-partition-drill-qemu
    test-trust-failure-drill-qemu
    test-update-failure-drill-qemu
    test-integration-replay-outage-drill-qemu
    test-scheduler-stress
    test-power-cycle-continuity-drill-qemu
)

if [[ -n "${HFOC_RESILIENCE_MATRIX_TARGETS:-}" ]]; then
    IFS=' ' read -r -a matrix_targets \
        <<<"$HFOC_RESILIENCE_MATRIX_TARGETS"
else
    matrix_targets=("${matrix_targets_default[@]}")
fi

(( ${#matrix_targets[@]} )) || {
    echo "Resilience matrix target list is empty" >&2
    exit 2
}

timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
artifact_dir=${HFOC_RESILIENCE_MATRIX_ARTIFACT_DIR:-}
if [[ -z "$artifact_dir" ]]; then
    artifact_dir="build/test-artifacts/resilience-matrix-$timestamp"
fi
mkdir -p "$artifact_dir"
manifest="$artifact_dir/matrix-manifest.txt"
suite_log="$artifact_dir/matrix-suite.log"
: >"$manifest"
: >"$suite_log"

args=()
for target in "${matrix_targets[@]}"; do
    args+=(--serial "$target")
done

if HFOC_MAKE_CMD="${HFOC_MAKE_CMD:-make}" \
    HFOC_QEMU_SUITE_ARTIFACT_DIR="$artifact_dir" \
    HFOC_QEMU_JOBS=1 \
    bash scripts/run-qemu-suite.sh "${args[@]}" >"$suite_log" 2>&1; then
    :
else
    status=$?
    failed_target=$(sed -n 's/^QEMU suite: FAIL //p' "$suite_log" |
        head -n 1)
    if [[ -n "$failed_target" ]]; then
        printf 'FAIL %s\n' "$failed_target" >>"$manifest"
    else
        printf 'FAIL unknown-target\n' >>"$manifest"
    fi
    printf 'FAILURE %s\n' "$status" >>"$manifest"
    cat "$suite_log" >&2
    echo "Resilience matrix failed with status $status" >&2
    exit "$status"
fi

for target in "${matrix_targets[@]}"; do
    log="$artifact_dir/$target.log"
    if [[ ! -f "$log" ]]; then
        printf 'FAIL missing-artifact %s\n' "$target" >>"$manifest"
        echo "Missing matrix artifact: $log" >&2
        exit 1
    fi
    if ! grep -Fq "QEMU suite: PASS $target" "$suite_log"; then
        printf 'FAIL missing-pass %s\n' "$target" >>"$manifest"
        echo "Missing QEMU suite pass marker for $target" >&2
        exit 1
    fi
    if grep -Eq '\*\*\* HFOC KERNEL PANIC \*\*\*|failed: ' "$log"; then
        printf 'FAIL panic-or-failure %s\n' "$target" >>"$manifest"
        echo "Panic or proof failure marker: $log" >&2
        exit 1
    fi
    printf 'PASS %s\n' "$target" >>"$manifest"
done

printf 'MATRIX OK\n' >>"$manifest"
echo "Resilience matrix artifacts: $artifact_dir"
echo "Resilience matrix: OK"
