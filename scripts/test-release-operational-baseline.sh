#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${HFOC_RELEASE_OPERATIONAL_MATRIX:-$ROOT/release/release-operational-baseline.txt}
MAKE_CMD=${HFOC_MAKE_CMD:-make}
ARTIFACT_DIR=${HFOC_RELEASE_OPERATIONAL_ARTIFACT_DIR:-}

[[ -f "$MATRIX" ]] || {
    echo "Missing release operational baseline: $MATRIX" >&2
    exit 1
}

bash "$ROOT/scripts/verify-release-operational-baseline.sh" "$MATRIX"

check_count=$(sed -n 's/^check_count=//p' "$MATRIX")
if [[ ! "$check_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Release operational baseline has invalid check count" >&2
    exit 1
fi

targets=()
for ((check_index = 1; check_index <= check_count; ++check_index)); do
    target=$(sed -n "s/^check_${check_index}_target=//p" "$MATRIX")
    if [[ -z "$target" || ! "$target" =~ ^test-[a-z0-9-]+$ ]]; then
        echo "Release operational baseline check ${check_index} has invalid target" >&2
        exit 1
    fi
    targets+=("$target")
done

if [[ -z "$ARTIFACT_DIR" ]]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    ARTIFACT_DIR="$ROOT/build/test-artifacts/release-operational-baseline-$timestamp"
fi
mkdir -p "$ARTIFACT_DIR"
manifest="$ARTIFACT_DIR/manifest.txt"
: >"$manifest"

for target in "${targets[@]}"; do
    log="$ARTIFACT_DIR/$target.log"
    set +e
    "$MAKE_CMD" "$target" >"$log" 2>&1
    status=$?
    set -e
    if (( status != 0 )); then
        printf 'FAIL %s\n' "$target" >>"$manifest"
        printf 'FAILURE %s\n' "$status" >>"$manifest"
        cat "$log" >&2
        echo "Release operational baseline failed at ${target} with status ${status}" >&2
        exit "$status"
    fi
    if ! grep -Fq 'ALL OK' "$log"; then
        printf 'FAIL %s\n' "$target" >>"$manifest"
        printf 'FAILURE missing-ok-marker\n' >>"$manifest"
        cat "$log" >&2
        echo "Release operational baseline target ${target} lacked its ALL OK marker" >&2
        exit 1
    fi
    printf 'PASS %s\n' "$target" >>"$manifest"
    echo "Release operational baseline: PASS $target"
done

printf 'OPERATIONAL BASELINE OK\n' >>"$manifest"
echo "Release operational baseline artifacts: $ARTIFACT_DIR"
echo "Release operational baseline: OK"
