#!/usr/bin/env bash
# SPDX-License-Identifier: AGPL-3.0-only OR LicenseRef-HFOC-Commercial
set -Eeuo pipefail
ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"
VERSION=$(cat VERSION)
TEST_DIR=$(mktemp -d)
cleanup() {
    chmod -R u+w "$TEST_DIR" 2>/dev/null || true
    rm -rf "$TEST_DIR"
}
trap cleanup EXIT

verify_iso() {
    local image=$1
    local name=$2
    local out="$TEST_DIR/$name"
    mkdir -p "$out"
    xorriso -osirrox on -indev "$image" -extract /licenses "$out/licenses" \
        >/dev/null 2>&1
    bash scripts/verify-license-payload.sh "$out/licenses"
}

verify_iso "build/hfoc-os-${VERSION}.iso" production
verify_iso "build/fallback/hfoc-os-${VERSION}-fallback.iso" recovery
echo 'License payload ISO host test: OK'
