#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MATRIX=${HFOC_INTEGRATED_ACCEPTANCE_MATRIX:-$ROOT/release/hfoc-integrated-installer-dual-boot-management-acceptance.txt}

bash "$ROOT/scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh" "$MATRIX"
echo "HFOC integrated installer/dual-boot/management acceptance gate: OK"
