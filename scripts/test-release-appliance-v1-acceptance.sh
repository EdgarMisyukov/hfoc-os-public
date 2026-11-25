#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${HFOC_APPLIANCE_V1_ACCEPTANCE_MATRIX:-$ROOT/release/release-appliance-v1-acceptance.txt}
MAKE_CMD=${HFOC_MAKE_CMD:-make}
ARTIFACT_DIR=${HFOC_APPLIANCE_V1_ACCEPTANCE_ARTIFACT_DIR:-}

[[ -f "$MATRIX" ]] || {
    echo "Missing appliance v1 acceptance matrix: $MATRIX" >&2
    exit 1
}

bash "$ROOT/scripts/verify-release-appliance-v1-acceptance.sh" "$MATRIX"

check_count=$(sed -n 's/^check_count=//p' "$MATRIX")
if [[ ! "$check_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Appliance v1 acceptance matrix has invalid check count" >&2
    exit 1
fi

targets=()
markers=()
for ((check_index = 1; check_index <= check_count; ++check_index)); do
    target=$(sed -n "s/^check_${check_index}_target=//p" "$MATRIX")
    marker=$(sed -n "s/^check_${check_index}_marker=//p" "$MATRIX")
    if [[ -z "$target" || ! "$target" =~ ^test-[a-z0-9-]+$ ]]; then
        echo "Appliance v1 acceptance matrix check ${check_index} has invalid target" >&2
        exit 1
    fi
    if [[ -z "$marker" ]]; then
        echo "Appliance v1 acceptance matrix check ${check_index} has invalid marker" >&2
        exit 1
    fi
    targets+=("$target")
    markers+=("$marker")
done

if [[ -z "$ARTIFACT_DIR" ]]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    ARTIFACT_DIR="$ROOT/build/test-artifacts/appliance-v1-acceptance-$timestamp"
fi
mkdir -p "$ARTIFACT_DIR"
manifest="$ARTIFACT_DIR/manifest.txt"

if HFOC_MAKE_CMD="$MAKE_CMD" bash "$ROOT/scripts/run-acceptance-suite.sh" \
    --artifact-dir "$ARTIFACT_DIR" \
    --parallel "${targets[0]}" "${markers[0]}" \
        "${targets[1]}" "${markers[1]}" \
    --serial "${targets[2]}" "${markers[2]}" \
    --serial "${targets[3]}" "${markers[3]}" \
    --parallel "${targets[4]}" "${markers[4]}" \
        "${targets[5]}" "${markers[5]}" \
        "${targets[6]}" "${markers[6]}" \
    --serial "${targets[7]}" "${markers[7]}" \
    --serial "${targets[8]}" "${markers[8]}"; then
    :
else
    status=$?
    echo "Appliance v1 acceptance gate: REJECT" >&2
    exit "$status"
fi

printf 'APPLIANCE V1 ACCEPTANCE OK\n' >>"$manifest"
echo "Appliance v1 acceptance gate artifacts: $ARTIFACT_DIR"
echo "Appliance v1 acceptance gate: ACCEPT"
