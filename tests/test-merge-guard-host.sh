#!/usr/bin/env bash
# ITEM-431: host proof for the merge guard.
#
# The guard exists because two post-merge runs on 2026-09-16 went red the same
# way: a pull request was merged while its own run was still in flight.  This
# proof drives the real tool against a fake `gh`, so every refusal - a live
# run, a head with no green run at all, a closed pull request, a head GitHub
# does not call clean - is shown to happen, and the one acceptance is shown to
# happen only when all of them are absent.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
GUARD="$ROOT/scripts/hfoc-merge-guard.sh"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-merge-guard-test.XXXXXX")
cleanup() {
    rm -rf "$WORK"
}
trap cleanup EXIT

checks=0
failures=0
fail() {
    printf 'merge guard host test: FAIL: %s\n' "$*" >&2
    exit 1
}
check() {
    local condition=$1
    local message=$2

    checks=$((checks + 1))
    if [[ $condition != 1 ]]; then
        failures=$((failures + 1))
        printf 'merge guard host test: FAIL: %s\n' "$message" >&2
    fi
}

# The fake `gh` answers the two reads the guard makes from files the cases
# write, so no case needs a network or a repository.
FAKE_GH=$WORK/gh
cat >"$FAKE_GH" <<'SCRIPT'
#!/usr/bin/env bash
set -Eeuo pipefail
printf '%s\n' "$*" >>"${HFOC_FAKE_ARGV_LOG:?}"
if [[ ${HFOC_FAKE_GH_FAIL:-0} == 1 ]]; then
    printf 'fake gh: read failure\n' >&2
    exit 1
fi
case "${1:-} ${2:-}" in
    'pr view')
        if [[ ${*:3} == *mergeCommit* ]]; then
            printf '{"mergeCommit":{"oid":"%s"}}\n' "${HFOC_FAKE_MERGE_COMMIT:-unknown}"
        else
            cat "${HFOC_FAKE_PR_JSON:?}"
        fi
        ;;
    'pr merge')
        if [[ ${HFOC_FAKE_MERGE_FAIL:-0} == 1 ]]; then
            printf 'fake gh: the merge was refused\n' >&2
            exit 1
        fi
        printf 'merged\n'
        ;;
    'run list')
        cat "${HFOC_FAKE_RUNS_JSON:?}"
        ;;
    *)
        printf 'fake gh: unsupported invocation: %s\n' "$*" >&2
        exit 1
        ;;
esac
SCRIPT
chmod +x "$FAKE_GH"
export HFOC_MERGE_GUARD_GH=$FAKE_GH

HFOC_FAKE_PR_JSON=$WORK/pr.json
HFOC_FAKE_RUNS_JSON=$WORK/runs.json
HFOC_FAKE_ARGV_LOG=$WORK/argv.log
export HFOC_FAKE_PR_JSON HFOC_FAKE_RUNS_JSON HFOC_FAKE_ARGV_LOG

write_pr() {
    cat >"$HFOC_FAKE_PR_JSON"
}

write_runs() {
    cat >"$HFOC_FAKE_RUNS_JSON"
}

# Case 1: a completed green run, no live run, mergeable and clean.
write_pr <<'JSON'
{"state":"OPEN","headRefOid":"1111111111111111111111111111111111111111","mergeable":"MERGEABLE","mergeStateStatus":"CLEAN"}
JSON
write_runs <<'JSON'
[{"databaseId":42,"status":"completed","conclusion":"success"}]
JSON
: >"$HFOC_FAKE_ARGV_LOG"
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 0 ]] && echo 1 || echo 0)" \
    "a green head with a clean pull request is accepted (exit $status)"
check "$([[ $out == *'state=OK run=42'* ]] && echo 1 || echo 0)" \
    "the acceptance names the run it accepted: $out"
check "$(grep -Fq 'pr view 7' "$HFOC_FAKE_ARGV_LOG" && echo 1 || echo 0)" \
    "the guard asks for the pull request it was given"
check "$(grep -Fq -- '--commit 1111111111111111111111111111111111111111' "$HFOC_FAKE_ARGV_LOG" && echo 1 || echo 0)" \
    "the guard asks for the runs of the head commit"
check "$(grep -Fq -- '--json databaseId,status,conclusion' "$HFOC_FAKE_ARGV_LOG" && echo 1 || echo 0)" \
    "the guard asks for the fields its verdict is made of"
check "$(grep -Fq -- '--json state,headRefOid,mergeable,mergeStateStatus' "$HFOC_FAKE_ARGV_LOG" && echo 1 || echo 0)" \
    "the guard asks for the pull-request fields its verdict is made of"

# Case 2: a run for the head is still live.
write_runs <<'JSON'
[{"databaseId":43,"status":"in_progress","conclusion":""}]
JSON
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 11 ]] && echo 1 || echo 0)" \
    "a live run is refused with exit 11 (exit $status)"
check "$([[ $out == *'reason=LIVE_RUN'* && $out == *'run=43'* ]] && echo 1 || echo 0)" \
    "the live-run refusal names the run: $out"

# Case 3: a live run next to an older green one stays refused; the older run
# must never paper over the newer one in flight (the exact 2026-09-16 shape).
write_runs <<'JSON'
[{"databaseId":41,"status":"completed","conclusion":"success"},
 {"databaseId":44,"status":"queued","conclusion":""}]
JSON
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 11 ]] && echo 1 || echo 0)" \
    "an older green run does not excuse a live one (exit $status)"
check "$([[ $out == *'run=44'* ]] && echo 1 || echo 0)" \
    "the refusal names the live run, not the green one: $out"

# Case 4: no completed successful run at all.
write_runs <<'JSON'
[{"databaseId":45,"status":"completed","conclusion":"failure"}]
JSON
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 12 ]] && echo 1 || echo 0)" \
    "a head with no green run is refused with exit 12 (exit $status)"
check "$([[ $out == *'reason=NO_GREEN_RUN'* ]] && echo 1 || echo 0)" \
    "the refusal names the missing green run: $out"

# Case 5: the pull request is not open.
write_pr <<'JSON'
{"state":"MERGED","headRefOid":"1111111111111111111111111111111111111111","mergeable":"MERGEABLE","mergeStateStatus":"CLEAN"}
JSON
write_runs <<'JSON'
[{"databaseId":42,"status":"completed","conclusion":"success"}]
JSON
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 10 ]] && echo 1 || echo 0)" \
    "a pull request that is not open is refused with exit 10 (exit $status)"
check "$([[ $out == *'reason=PR_NOT_OPEN'* ]] && echo 1 || echo 0)" \
    "the refusal names the closed pull request: $out"

# Case 6: GitHub does not call the head clean.
write_pr <<'JSON'
{"state":"OPEN","headRefOid":"1111111111111111111111111111111111111111","mergeable":"CONFLICTING","mergeStateStatus":"DIRTY"}
JSON
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 13 ]] && echo 1 || echo 0)" \
    "a head GitHub does not call clean is refused with exit 13 (exit $status)"
check "$([[ $out == *'reason=NOT_CLEAN'* && $out == *'merge_state=DIRTY'* ]] && echo 1 || echo 0)" \
    "the refusal names the merge state: $out"

# Case 6b: every status the Actions API can report for a run that has not
# finished is live, not only the two common ones.
write_pr <<'JSON'
{"state":"OPEN","headRefOid":"1111111111111111111111111111111111111111","mergeable":"MERGEABLE","mergeStateStatus":"CLEAN"}
JSON
for rare_status in pending requested waiting; do
    write_runs <<JSON
[{"databaseId":46,"status":"$rare_status","conclusion":""}]
JSON
    out=$("$GUARD" 7 2>&1) && status=0 || status=$?
    check "$([[ $status -eq 11 ]] && echo 1 || echo 0)" \
        "a $rare_status run is refused with exit 11 (exit $status)"
done

# Case 6c: a completed run without an id is not a green run to accept.
write_runs <<'JSON'
[{"databaseId":null,"status":"completed","conclusion":"success"}]
JSON
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
check "$([[ $status -eq 12 ]] && echo 1 || echo 0)" \
    "a green run without an id is refused with exit 12 (exit $status)"

# Case 6e: with --merge, the verdict and the merge are one step; a refusal
# never reaches the merge at all.
write_pr <<'JSON'
{"state":"OPEN","headRefOid":"1111111111111111111111111111111111111111","mergeable":"MERGEABLE","mergeStateStatus":"CLEAN"}
JSON
write_runs <<'JSON'
[{"databaseId":42,"status":"completed","conclusion":"success"}]
JSON
: >"$HFOC_FAKE_ARGV_LOG"
export HFOC_FAKE_MERGE_COMMIT=abc1234
out=$("$GUARD" 7 --merge 2>&1) && status=0 || status=$?
unset HFOC_FAKE_MERGE_COMMIT
check "$([[ $status -eq 0 ]] && echo 1 || echo 0)" \
    "a green head merges with --merge (exit $status)"
check "$([[ $out == *'state=OK run=42'* && $out == *'state=MERGED commit=abc1234'* ]] && echo 1 || echo 0)" \
    "the merged verdict names the commit: $out"
check "$(grep -Fq 'pr merge 7 --squash' "$HFOC_FAKE_ARGV_LOG" && echo 1 || echo 0)" \
    "the guard merges the pull request it was given, squash-style"

write_runs <<'JSON'
[{"databaseId":43,"status":"in_progress","conclusion":""}]
JSON
: >"$HFOC_FAKE_ARGV_LOG"
out=$("$GUARD" 7 --merge 2>&1) && status=0 || status=$?
check "$([[ $status -eq 11 ]] && echo 1 || echo 0)" \
    "a live run refuses --merge before merging (exit $status)"
check "$(! grep -Fq 'pr merge 7' "$HFOC_FAKE_ARGV_LOG" && echo 1 || echo 0)" \
    "the live-run refusal never reaches the merge: $out"

# Case 6f: a merge GitHub refuses (a run appeared in between, a protected
# branch) is its own code, and no MERGED verdict is printed.
write_pr <<'JSON'
{"state":"OPEN","headRefOid":"1111111111111111111111111111111111111111","mergeable":"MERGEABLE","mergeStateStatus":"CLEAN"}
JSON
write_runs <<'JSON'
[{"databaseId":42,"status":"completed","conclusion":"success"}]
JSON
export HFOC_FAKE_MERGE_FAIL=1
out=$("$GUARD" 7 --merge 2>&1) && status=0 || status=$?
unset HFOC_FAKE_MERGE_FAIL
check "$([[ $status -eq 66 ]] && echo 1 || echo 0)" \
    "a refused merge is its own code (exit $status)"
check "$([[ $out == *'reason=MERGE_REFUSED'* && $out != *'state=MERGED'* ]] && echo 1 || echo 0)" \
    "the refusal never claims the merge happened: $out"

# Case 6d: a gh that cannot be read is its own code and its own marker.
export HFOC_FAKE_GH_FAIL=1
out=$("$GUARD" 7 2>&1) && status=0 || status=$?
unset HFOC_FAKE_GH_FAIL
check "$([[ $status -eq 65 ]] && echo 1 || echo 0)" \
    "a gh read failure is refused with exit 65 (exit $status)"
check "$([[ $out == *'state=REFUSED reason=GH_UNREADABLE'* ]] && echo 1 || echo 0)" \
    "the read failure prints the marker: $out"

# Case 7: usage errors are their own code, and no read happens without them.
out=$("$GUARD" 2>&1) && status=0 || status=$?
check "$([[ $status -eq 64 ]] && echo 1 || echo 0)" \
    "a missing pull request number is a usage error (exit $status)"
out=$("$GUARD" seven 2>&1) && status=0 || status=$?
check "$([[ $status -eq 64 ]] && echo 1 || echo 0)" \
    "a non-numeric pull request number is a usage error (exit $status)"

if [[ $failures -ne 0 ]]; then
    printf 'merge guard host test: %d checks, %d failures\n' \
        "$checks" "$failures"
    exit 1
fi
printf 'merge guard host test: OK (%d checks)\n' "$checks"
