#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MATRIX=${HFOC_APPLIANCE_PLATFORM_RELEASE_ACCEPTANCE_MATRIX:-$ROOT/release/hfoc-appliance-platform-release-acceptance.txt}

bash "$ROOT/scripts/verify-hfoc-appliance-platform-release-acceptance.sh" "$MATRIX"
echo "HFOC Appliance Platform release acceptance gate: OK"
