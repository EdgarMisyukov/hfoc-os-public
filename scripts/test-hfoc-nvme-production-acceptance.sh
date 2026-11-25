#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
MATRIX=${HFOC_NVME_PRODUCTION_ACCEPTANCE_MATRIX:-$ROOT/release/hfoc-nvme-production-acceptance.txt}

bash "$ROOT/scripts/verify-hfoc-nvme-production-acceptance.sh" "$MATRIX"
echo "HFOC NVMe production acceptance gate: OK"
