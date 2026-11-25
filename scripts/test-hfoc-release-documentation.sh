#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
bash "$ROOT/scripts/verify-hfoc-release-documentation.sh" "$ROOT"
echo "HFOC release documentation gate: OK"
