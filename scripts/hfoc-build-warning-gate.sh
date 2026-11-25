#!/usr/bin/env bash
# ITEM-432: the build-warning gate.
#
# The production build carried exactly two warnings for several items - a dead
# declaration in kernel/src/platform_reset.c and a function declared unable to
# return that the compiler could not prove - and nothing failed on them,
# because no phase read the build it produced.  This is the reader: it takes
# one captured build log and answers whether our own code compiled clean.
#
#   HFOC-BUILD-WARNINGS state=CLEAN kernel=0
#   HFOC-BUILD-WARNINGS state=FAIL kernel=<n>          (the lines go to stderr)
#   HFOC-BUILD-WARNINGS state=FAIL reason=LOG_UNREADABLE|LOG_EMPTY
#
# Exit codes:
#   0   no warning naming a `kernel/` file
#   1   at least one such warning, or the log was missing, unreadable or empty
#   64  usage error
#
# The pattern is GCC's own line shape for our sources, with an optional column
# so assembler diagnostics ("Warning:", no column) are counted too.  Only
# `kernel/` paths are counted: a third-party or host-tool warning must not
# block a phase, and a `kernel/` warning must.
set -Eeuo pipefail

readonly EX_USAGE=64
readonly EX_WARNING=1

readonly WARNING_PATTERN='^kernel/[^:]+:[0-9]+(:[0-9]+)?: [Ww]arning:'

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
}

if [[ $# -ne 1 || $1 == -h || $1 == --help ]]; then
    usage >&2
    exit "$EX_USAGE"
fi
log=$1
[[ -f $log ]] || {
    echo "HFOC-BUILD-WARNINGS state=FAIL reason=LOG_UNREADABLE" >&2
    exit "$EX_WARNING"
}
# A log with no bytes is not a build that produced no warning: it is a build
# whose output never arrived, and the gate must not call that clean.
if [[ ! -s $log ]]; then
    echo "HFOC-BUILD-WARNINGS state=FAIL reason=LOG_EMPTY" >&2
    exit "$EX_WARNING"
fi

# grep exits 1 for "no match" and 2 for "the log could not be read"; the
# second is a failure of the gate itself, never a clean verdict.
set +e
warning_lines=$(grep -E "$WARNING_PATTERN" "$log")
warning_status=$?
set -e
if (( warning_status > 1 )); then
    echo "HFOC-BUILD-WARNINGS state=FAIL reason=LOG_UNREADABLE" >&2
    exit "$EX_WARNING"
fi
if (( warning_status == 0 )); then
    printf '%s\n' "$warning_lines" >&2
    warning_count=$(printf '%s\n' "$warning_lines" | grep -c .)
    echo "HFOC-BUILD-WARNINGS state=FAIL kernel=$warning_count" >&2
    exit "$EX_WARNING"
fi
echo "HFOC-BUILD-WARNINGS state=CLEAN kernel=0"
