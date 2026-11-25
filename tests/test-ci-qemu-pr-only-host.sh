#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
workflow="$ROOT/.github/workflows/ci.yml"

grep -Fqx '    types: [opened, synchronize, reopened, labeled, unlabeled]' \
    "$workflow" || {
    echo "Pull-request CI must not start merely when a draft is marked ready" >&2
    exit 1
}

qemu_step=$(awk '
  /^      - name: Build and QEMU smoke test$/ { capture=1 }
  capture { print }
  capture && /^      - name: / && $0 != "      - name: Build and QEMU smoke test" { exit }
' "$workflow")

grep -Fqx "        if: github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch'" <<<"$qemu_step" || {
    echo "QEMU step must run only for pull_request or manual-dispatch events" >&2
    exit 1
}

grep -Fqx '          make test' <<<"$qemu_step" || {
    echo "QEMU step must execute make test" >&2
    exit 1
}

echo "CI QEMU PR-only host test: OK"
