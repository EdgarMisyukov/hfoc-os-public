#!/usr/bin/env bash
# ITEM-462: host proof for the CI diagnostics budget.
#
# The diagnostics artifact exhausted the account's GitHub Actions storage, so
# the three rules that bound it are driven here with real directories and a
# mutated workflow:
#
#   * `cleanup` removes only directories older than the retention and publishes
#     the count and the bytes it removed;
#   * `collect` copies text evidence, keeps a large log as its tail, never
#     copies a raw image or an ISO, names each excluded file, and stops at the
#     byte budget;
#   * `guard` accepts the real workflow and rejects one that ships the raw tree
#     unqualified, drops an exclusion, keeps the artifact for 90 days, or omits
#     the collector or the cleanup.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
TOOL="$ROOT/scripts/hfoc-ci-artifacts.sh"
WORKFLOW="$ROOT/.github/workflows/ci.yml"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-ci-artifacts-test.XXXXXX")
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

checks=0
failures=0
fail() {
    printf 'ci artifacts host test: FAIL: %s\n' "$*" >&2
    exit 1
}
check() {
    local condition=$1
    local message=$2

    checks=$((checks + 1))
    if [[ $condition != 1 ]]; then
        failures=$((failures + 1))
        printf 'ci artifacts host test: FAIL: %s\n' "$message" >&2
    fi
}
run() {
    local expected_status=$1
    shift
    out=$("$@" 2>&1) && status=0 || status=$?
    check "$([[ $status -eq $expected_status ]] && echo 1 || echo 0)" \
        "exit $status, expected $expected_status: $out"
}
expect_marker() {
    check "$([[ $out == *"$1"* ]] && echo 1 || echo 0)" \
        "marker missing '$1': $out"
}

[[ -x $TOOL ]] || fail "not executable: $TOOL"
[[ -f $WORKFLOW ]] || fail "missing workflow: $WORKFLOW"

# ---------------------------------------------------------------- guard ------
run 0 bash "$TOOL" guard --workflow "$WORKFLOW"
expect_marker '[CI-ARTIFACTS] guard'
expect_marker 'state=OK'

# The smallest workflow the guard accepts, so each mutation below is the only
# difference: it carries the collector, the cleanup and the upload exclusions.
cat >"$WORK/good.yml" <<'YAML'
      - name: Clean old test artifacts
        run: scripts/hfoc-ci-artifacts.sh cleanup --root build/test-artifacts --days 7
      - name: Collect CI diagnostics
        run: scripts/hfoc-ci-artifacts.sh collect --root build/test-artifacts --dest build/ci-diagnostics --budget-bytes 33554432
      - name: Upload diagnostics
        uses: actions/upload-artifact@v7
        with:
          retention-days: 7
          path: |
            build/ci-diagnostics/**
            !**/*.raw
            !**/*.img
            !build/hfoc-os-*.iso
YAML

run 0 bash "$TOOL" guard --workflow "$WORK/good.yml"
expect_marker 'state=OK'

guard_fixture() {
    local label=$1
    local reason=$2

    run 1 bash "$TOOL" guard --workflow "$WORK/fixture.yml"
    expect_marker 'state=FAIL'
    expect_marker "$reason"
    check 1 "$label"
}

# 1. The pre-ITEM-462 path: the raw tree shipped unqualified.
sed 's|            build/ci-diagnostics/\*\*|            build/test-artifacts/**|' \
    "$WORK/good.yml" >"$WORK/fixture.yml"
guard_fixture 'unqualified build/test-artifacts is rejected' \
    TEST_ARTIFACTS_UNQUALIFIED

# 2. An exclusion removed.
grep -Fv '!**/*.raw' "$WORK/good.yml" >"$WORK/fixture.yml"
guard_fixture 'a missing raw-image exclusion is rejected' \
    'EXCLUSION_MISSING:!**/*.raw'

# 3. The retention the repository actually had before the item.
sed 's|retention-days: 7|retention-days: 90|' "$WORK/good.yml" >"$WORK/fixture.yml"
guard_fixture 'a 90-day retention is rejected' 'RETENTION_TOO_LONG:90'

# 4. The collector removed.
grep -Fv 'scripts/hfoc-ci-artifacts.sh collect' "$WORK/good.yml" >"$WORK/fixture.yml"
guard_fixture 'a missing collector is rejected' COLLECT_STEP_MISSING

# 5. The cleanup removed.
grep -Fv 'scripts/hfoc-ci-artifacts.sh cleanup' "$WORK/good.yml" >"$WORK/fixture.yml"
guard_fixture 'a missing cleanup is rejected' CLEANUP_STEP_MISSING

# 6. A missing workflow is a refusal, not a crash.
run 1 bash "$TOOL" guard --workflow "$WORK/absent.yml"
expect_marker 'WORKFLOW_MISSING'

# -------------------------------------------------------------- cleanup ------
artifacts="$WORK/artifacts"
mkdir -p "$artifacts/old-run" "$artifacts/fresh-run"
printf 'old evidence\n' >"$artifacts/old-run/probe.log"
# 64 KiB of raw image, the shape that used to be uploaded.
head -c 65536 /dev/zero >"$artifacts/old-run/disk.raw"
printf 'fresh evidence\n' >"$artifacts/fresh-run/probe.log"
touch -d '10 days ago' "$artifacts/old-run"

run 0 bash "$TOOL" cleanup --root "$artifacts" --days 3
expect_marker 'removed_dirs=1'
check "$([[ ! -d $artifacts/old-run ]] && echo 1 || echo 0)" \
    'the old directory was not removed'
check "$([[ -d $artifacts/fresh-run ]] && echo 1 || echo 0)" \
    'the fresh directory was removed'
# The reported bytes must cover the removed raw image, not just the log.
removed_bytes=$(sed -n 's/.*removed_bytes=\([0-9]*\).*/\1/p' <<<"$out")
check "$([[ ${removed_bytes:-0} -ge 65536 ]] && echo 1 || echo 0)" \
    "removed_bytes=$removed_bytes does not name the removed image"

# A missing root is not an error: the first run on a clean host removes nothing.
run 0 bash "$TOOL" cleanup --root "$WORK/absent" --days 3
expect_marker 'removed_dirs=0'

# -------------------------------------------------------------- collect ------
source_root="$WORK/collect-src"
mkdir -p "$source_root/probe-a" "$source_root/probe-b"
printf 'first log line\n' >"$source_root/probe-a/test-qemu-proof.log"
# One log larger than the per-file tail limit: only its tail is kept.
{
    head -c 1024 /dev/zero | tr '\0' 'A'
    printf '\nLAST-LINE\n'
} >"$source_root/probe-a/huge.log"
printf 'the summary\n' >"$source_root/probe-b/probe.txt"
printf '#!/bin/sh\necho hi\n' >"$source_root/probe-b/setup.sh"
head -c 131072 /dev/zero >"$source_root/probe-b/disk.raw"
printf 'ISO-9660' >"$source_root/probe-b/hfoc-os-0.2.6.iso"

dest="$WORK/collected"
run 0 bash "$TOOL" collect --root "$source_root" --dest "$dest" \
    --budget-bytes 1048576 --per-file-bytes 4096
expect_marker '[CI-ARTIFACTS] collect'
expect_marker 'excluded='
check "$([[ -f $dest/probe-a/test-qemu-proof.log ]] && echo 1 || echo 0)" \
    'the log was not collected'
check "$([[ -f $dest/probe-b/probe.txt ]] && echo 1 || echo 0)" \
    'the summary was not collected'
check "$([[ ! -e $dest/probe-b/disk.raw ]] && echo 1 || echo 0)" \
    'the raw image was collected'
check "$([[ ! -e $dest/probe-b/hfoc-os-0.2.6.iso ]] && echo 1 || echo 0)" \
    'the ISO was collected'
check "$([[ ! -e $dest/probe-b/setup.sh ]] && echo 1 || echo 0)" \
    'a non-evidence script was collected'
grep -Fq 'disk.raw' "$dest/EXCLUDED.txt" || fail 'EXCLUDED.txt does not name disk.raw'
grep -Fq 'hfoc-os-0.2.6.iso' "$dest/EXCLUDED.txt" || fail 'EXCLUDED.txt does not name the ISO'
grep -Fq 'LAST-LINE' "$dest/probe-a/huge.log" || fail 'the tail of a large log was lost'
check "$([[ $(wc -c <"$dest/probe-a/huge.log") -le 4096 ]] && echo 1 || echo 0)" \
    'a large log was not bounded by the per-file limit'

# The budget stops the collection and is published.
dest_small="$WORK/collected-small"
run 0 bash "$TOOL" collect --root "$source_root" --dest "$dest_small" \
    --budget-bytes 16 --per-file-bytes 4096
expect_marker 'budget_exhausted=1'
check "$([[ $(du -sb "$dest_small" | awk '{print $1}') -lt 1048576 ]] && echo 1 || echo 0)" \
    'the small budget still produced a large directory'

# `--since` keeps only evidence touched by this phase.
dest_since="$WORK/collected-since"
future=$(( $(date +%s) + 3600 ))
run 0 bash "$TOOL" collect --root "$source_root" --dest "$dest_since" \
    --budget-bytes 1048576 --since "$future"
check "$([[ $(find "$dest_since" -type f ! -name EXCLUDED.txt | wc -l) -eq 0 ]] && echo 1 || echo 0)" \
    '--since collected files older than the phase'

# Usage errors are their own code.
run 2 bash "$TOOL"
run 2 bash "$TOOL" cleanup --root "$WORK" --days x

if [[ $failures -ne 0 ]]; then
    printf 'ci artifacts host test: %d checks, %d failures\n' \
        "$checks" "$failures"
    exit 1
fi
printf 'ci artifacts host test: OK (%d checks)\n' "$checks"
