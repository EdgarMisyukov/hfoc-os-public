#!/usr/bin/env bash
# ITEM-641: answer "does this file carry this fixed marker?" in one process.
#
# The old `strings -a "$kernel" | grep -Fq "$marker"` pipeline reports 141 under
# `set -Eeuo pipefail` when grep exits at the first match and strings is still
# writing.  A present build id then looked absent, and the same shape made a
# negative assertion fail open: `if strings ... | grep -Fq forbidden; then
# refuse; fi` never refused a kernel that really carried the marker.
#
# grep -aFq reads the bytes directly: no producer to kill, no SIGPIPE, and the
# caller only has to look at one exit status.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

if (( $# != 2 )); then
    echo "usage: $0 <file> <fixed-marker>" >&2
    exit 2
fi

file=$1
marker=$2

[[ -f $file && -r $file ]] || {
    echo "file marker check: unreadable file: $file" >&2
    exit 2
}
[[ -n $marker ]] || {
    echo "file marker check: empty marker" >&2
    exit 2
}

status=0
grep -aFq -- "$marker" "$file" || status=$?
case $status in
    0) exit 0 ;;
    1) exit 1 ;;
    *)
        echo "file marker check: cannot read $file" >&2
        exit 2
        ;;
esac
