#!/usr/bin/env bash
# ITEM-585: bounded CI timing artifact.
#
# Every runner appends one tab-separated line per finished unit:
#   job  attempt  sha  phase  kind  name  status  rc  ms  retry  started_ms
# `ms` is measured with /proc/uptime (monotonic), never wall-clock.  Lines are
# appended under flock, one write per line, and every run writes into its own
# `<job>-<run>-<attempt>` directory, so two parallel phases or attempts can
# never interleave records or read a previous run's data.
#
# This script only reads: it prints a bounded summary (top-20 by duration,
# failures included) and writes it beside the records.  A summary never claims
# that the sum of parallel durations is the phase time; the phase wrapper
# records the elapsed phase separately under kind=phase-wrapper.
set -Eeuo pipefail

root=${HFOC_CI_TIMINGS_DIR:-build/test-artifacts/ci-timings}
top=20

usage() {
    echo "usage: hfoc-ci-timings.sh summarize [--dir DIR] [--top N]" >&2
    exit 2
}

(( $# > 0 )) || usage
command=$1
shift
case $command in
    summarize) ;;
    *) usage ;;
esac
while (( $# )); do
    case $1 in
        --dir) root=$2; shift 2 ;;
        --top) top=$2; shift 2 ;;
        *) usage ;;
    esac
done
[[ $top =~ ^[1-9][0-9]*$ ]] || usage

[[ -d $root ]] || {
    echo "CI-TIMINGS: no records at $root"
    exit 0
}

report=$(mktemp "${TMPDIR:-/tmp}/hfoc-ci-timings.XXXXXX")
data=$(mktemp "${TMPDIR:-/tmp}/hfoc-ci-timings-data.XXXXXX")
trap 'rm -f "$report" "$data"' EXIT

# The summary is produced from every record line that has the full schema.
# A malformed line is counted and named, never silently used.
records=0
malformed=0

{
    printf 'CI-TIMINGS schema=1 dir=%s\n' "$root"
    while IFS= read -r file; do
        while IFS=$'\t' read -r job attempt sha phase kind name status rc ms retry _ ||
            [[ -n ${job:-} ]]; do
            [[ -z $job || $job == job ]] && continue
            if [[ -z $kind || -z $name || -z $status || ! $ms =~ ^[0-9]+$ ]]; then
                malformed=$((malformed + 1))
                continue
            fi
            seconds=$(awk -v ms="$ms" 'BEGIN { printf "%.3f", ms / 1000 }')
            printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
                "$kind" "$name" "$status" "$rc" "$seconds" "$retry" \
                "$phase" "$job" "$attempt" "$sha"
            records=$((records + 1))
        done <"$file"
    done < <(find "$root" -type f -name '*.tsv' -print 2>/dev/null | sort)
} >"$data"

{
    printf 'kind\tname\tstatus\trc\tseconds\tretry\tphase\tjob\tattempt\tsha\n'
    cat "$data"
    printf '\nTOP %d by duration (failures included):\n' "$top"
    printf 'seconds\tkind\tname\tstatus\trc\tphase\tattempt\n'
    awk -F'\t' '$5 != "" { printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\n", $5, $1, $2, $3, $4, $7, $9 }' \
        "$data" | sort -rn | sed -n "1,${top}p"
    printf '\nBY KIND (count, total seconds; a parallel sum is not phase time):\n'
    awk -F'\t' '$5 != "" { count[$1]++; total[$1] += $5 } END { for (k in count) printf "%s\t%d\t%.3f\n", k, count[k], total[k] }' \
        "$data" | sort
    printf '\nPHASE WRAPPERS (wall time per phase, own kind):\n'
    awk -F'\t' '$1 == "phase-wrapper" { printf "%s\t%s\t%s\t%s\n", $2, $3, $5, $6 }' \
        "$data" | sort
    printf '\nrecords=%d malformed=%d\n' "$records" "$malformed"
    if (( malformed != 0 )); then
        printf 'CI-TIMINGS: %d malformed record(s) were not used\n' "$malformed"
    fi
} >"$report"

cat "$report"
cp "$report" "$root/SUMMARY.txt" 2>/dev/null || true
exit 0
