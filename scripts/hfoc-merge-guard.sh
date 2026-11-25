#!/usr/bin/env bash
# ITEM-431: the merge guard.
#
# Two red post-merge runs on 2026-09-16 came from the same mistake - a pull
# request was merged while its own CI run was still in flight - and the rule
# that prevents it lived only in the worker's prompt: "do not merge until the
# PR's own run has completed".  This tool makes the rule checkable, so a merge
# is refused by something other than memory.
#
# It reads the pull request and the runs for its head commit through `gh`, and
# answers with one marker line and a distinct exit code:
#
#   HFOC-MERGE-GUARD pr=<n> head=<sha> state=OK run=<id>
#   HFOC-MERGE-GUARD pr=<n> head=<sha> state=REFUSED reason=<name> [detail=...]
#
# With `--merge`, the verdict and the merge are one step: a clean verdict is
# followed by `gh pr merge <n> --squash` and
#
#   HFOC-MERGE-GUARD pr=<n> head=<sha> state=MERGED commit=<sha>
#
# while a refusal never reaches the merge at all.  The point is that the rule
# is checked in the same breath as the action it guards: there is no window
# between "the guard said yes" and "the merge happened" for a new run to
# appear in.
#
# Exit codes:
#   0   the head has a completed successful run, no run for it is live, and
#       GitHub reports the pull request mergeable and clean
#   10  the pull request is not open
#   11  a run for this head is queued or in progress (the live-run refusal)
#   12  no completed successful run exists for this head
#   13  GitHub does not report the pull request mergeable and clean
#   65  `gh` could not be read (network, auth, or an answer without a head)
#   66  the merge itself was refused (a new run, a protected branch, a race)
#   64  usage error, or the `gh` binary is missing
#
# Every refusal prints the marker, including the `gh` read failures: a caller
# must be able to tell "the guard looked and refused" from "the guard never
# ran", which is why a read failure is 65 and not 64.
#
# `HFOC_MERGE_GUARD_GH` points at another `gh` binary, which is how the host
# proof drives every refusal without a network.
set -Eeuo pipefail

readonly EX_USAGE=64
readonly EX_GH_UNREADABLE=65
readonly EX_MERGE_REFUSED=66
readonly EX_CLOSED=10
readonly EX_LIVE_RUN=11
readonly EX_NO_GREEN_RUN=12
readonly EX_NOT_CLEAN=13

gh_bin=${HFOC_MERGE_GUARD_GH:-gh}

usage() {
    sed -n '2,44p' "$0" | sed 's/^# \{0,1\}//'
}

unreadable() {
    local detail=$1

    head=${head:-unknown}
    printf 'HFOC-MERGE-GUARD pr=%s head=%s state=REFUSED reason=GH_UNREADABLE detail=%s\n' \
        "$pr" "$head" "$detail"
    exit "$EX_GH_UNREADABLE"
}

refuse() {
    local code=$1
    local reason=$2
    shift 2
    printf 'HFOC-MERGE-GUARD pr=%s head=%s state=REFUSED reason=%s' \
        "$pr" "$head" "$reason"
    local pair
    for pair in "$@"; do
        printf ' %s' "$pair"
    done
    printf '\n'
    exit "$code"
}

merge_after=0
if [[ ${2:-} == --merge ]]; then
    merge_after=1
fi
if [[ $# -lt 1 || $# -gt 2 || $1 == -h || $1 == --help ]]; then
    usage >&2
    exit "$EX_USAGE"
fi
if [[ $# -eq 2 && $merge_after -eq 0 ]]; then
    usage >&2
    exit "$EX_USAGE"
fi
pr=$1
case $pr in
    '' | *[!0-9]*)
        printf 'hfoc-merge-guard: the pull request number must be digits\n' >&2
        exit "$EX_USAGE"
        ;;
esac
command -v "$gh_bin" >/dev/null 2>&1 || {
    printf 'hfoc-merge-guard: %s is not on PATH\n' "$gh_bin" >&2
    exit "$EX_USAGE"
}

field() {
    local json=$1
    local name=$2
    python3 -c '
import json
import sys

name = sys.argv[1]
try:
    document = json.load(sys.stdin)
except ValueError:
    print("")
    raise SystemExit(0)
value = document.get(name, "") if isinstance(document, dict) else ""
print("" if value is None else value)
' "$name" <<<"$json"
}

if ! pr_json=$("$gh_bin" pr view "$pr" \
    --json state,headRefOid,mergeable,mergeStateStatus 2>/dev/null); then
    head=unknown
    unreadable "read=pr_view"
fi
head=$(field "$pr_json" headRefOid)
state=$(field "$pr_json" state)
mergeable=$(field "$pr_json" mergeable)
merge_status=$(field "$pr_json" mergeStateStatus)
[[ $head =~ ^[0-9a-f]{40}$ ]] || unreadable "read=head_oid"

if [[ $state != OPEN ]]; then
    refuse "$EX_CLOSED" PR_NOT_OPEN "state=$state"
fi

if ! runs_json=$("$gh_bin" run list --commit "$head" --limit 50 \
    --json databaseId,status,conclusion 2>/dev/null); then
    unreadable "read=run_list"
fi

# The two answers are the whole rule: a run of this head that has not finished
# (the finding that produced the red `main` runs), and a run of this head that
# has finished green.  The live run is refused first, because an older green
# run must never paper over a newer one still in flight.
selected=$(python3 -c '
import json
import sys

LIVE = {"queued", "in_progress", "pending", "requested", "waiting"}
try:
    runs = json.load(sys.stdin)
except ValueError:
    runs = []
if not isinstance(runs, list):
    runs = []
live = ""
green = ""
for run in runs:
    if not isinstance(run, dict):
        continue
    run_id = run.get("databaseId")
    status = run.get("status", "")
    conclusion = run.get("conclusion", "")
    if status in LIVE and not live:
        live = str(run_id) if isinstance(run_id, int) else "unknown"
    if (status == "completed" and conclusion == "success" and
            isinstance(run_id, int) and not green):
        green = str(run_id)
print(live)
print(green)
' <<<"$runs_json")
live_run=$(sed -n '1p' <<<"$selected")
green_run=$(sed -n '2p' <<<"$selected")

if [[ -n $live_run ]]; then
    refuse "$EX_LIVE_RUN" LIVE_RUN "run=$live_run"
fi
if [[ -z $green_run ]]; then
    refuse "$EX_NO_GREEN_RUN" NO_GREEN_RUN "completed=0"
fi
if [[ $mergeable != MERGEABLE || $merge_status != CLEAN ]]; then
    refuse "$EX_NOT_CLEAN" NOT_CLEAN \
        "mergeable=$mergeable" "merge_state=$merge_status"
fi

printf 'HFOC-MERGE-GUARD pr=%s head=%s state=OK run=%s\n' \
    "$pr" "$head" "$green_run"
if (( merge_after == 0 )); then
    exit 0
fi
if ! merge_error=$("$gh_bin" pr merge "$pr" --squash 2>&1 >/dev/null); then
    printf 'HFOC-MERGE-GUARD pr=%s head=%s state=REFUSED reason=MERGE_REFUSED detail=%s\n' \
        "$pr" "$head" "${merge_error:-unknown}"
    exit "$EX_MERGE_REFUSED"
fi
merge_commit=unknown
if merged_json=$("$gh_bin" pr view "$pr" --json mergeCommit 2>/dev/null); then
    merge_commit=$(python3 -c '
import json
import sys

try:
    document = json.load(sys.stdin)
except ValueError:
    document = {}
commit = document.get("mergeCommit") if isinstance(document, dict) else None
oid = commit.get("oid", "") if isinstance(commit, dict) else ""
print(oid if oid else "unknown")
' <<<"$merged_json")
fi
printf 'HFOC-MERGE-GUARD pr=%s head=%s state=MERGED commit=%s\n' \
    "$pr" "$head" "$merge_commit"
