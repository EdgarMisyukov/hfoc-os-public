#!/usr/bin/env bash
# ITEM-422: which proofs the acceptance phase runs, and whether each of them
# declares the class of a marker timeout.
#
# ITEM-412 and ITEM-421 gave the phase a rule - a proof that stopped because it
# did not see an expected marker inside its own deadline declares that class,
# and the phase retries it once - and taught two proofs to declare it.  The rule
# only works for a proof that declares, and on 2026-09-15 the gate failed on
# `test-pic-disable`, whose stop was exactly that class and carried no record:
# the classifier read the silence as an assertion, which is never retried.
#
# So the declaration is required of every proof the phase runs, not of the ones
# that have flaked so far:
#
#   * the target list is the phase's own list, read out of the Makefile, so a
#     target joins it by being run and not by being remembered here;
#   * each target's script must write the canonical record
#     (`HFOC-QEMU-MARKER-TIMEOUT`), and each give-up on a guest marker inside it
#     must have a record immediately before it;
#   * a wait for the harness rather than for the guest - a monitor or serial
#     socket - is not a marker timeout and is not required to declare one.
#
# Usage:
#   hfoc-proof-marker-class.sh list              # target<TAB>script, one a line
#   hfoc-proof-marker-class.sh check [script...] # 0 when every script declares
#
# `check` reads the tree by default; naming scripts checks exactly those, which
# is how scripts/check.d/45-qemu-marker-timeout-retry.sh drills the rule against
# a script that does not declare.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
makefile=$root/Makefile

# The record the classifier reads, and the canonical spelling of it.
readonly RECORD=HFOC-QEMU-MARKER-TIMEOUT

fail() {
    echo "proof marker class: $*" >&2
    return 1
}

phase_targets() {
    awk '
        /^QEMU_SERIAL_TARGETS[[:space:]]*:?=/ { inside = 1; next }
        /^QEMU_PARALLEL_PHASE_1[[:space:]]*:?=/ { inside = 1; next }
        inside && /^[[:space:]]*$/ { inside = 0; next }
        inside && /^[A-Za-z_]+[[:space:]]*:?=/ { inside = 0 }
        inside {
            line = $0
            sub(/\\$/, "", line)
            gsub(/^[[:space:]]+|[[:space:]]+$/, "", line)
            if (line != "" && line !~ /^\$/) { print line }
        }
    ' "$makefile"
}

script_of() {
    awk -v target="$1" '
        /^[A-Za-z0-9_.-]+:/ {
            split($0, parts, ":")
            current = parts[1]
        }
        {
            if (current == target && match($0, /scripts\/[A-Za-z0-9_.-]+\.sh/)) {
                print substr($0, RSTART, RLENGTH)
                exit
            }
        }
    ' "$makefile"
}

pairs() {
    local target script
    while IFS= read -r target; do
        script=$(script_of "$target")
        [[ -n $script ]] && printf '%s\t%s\n' "$target" "$script"
    done < <(phase_targets)
}

# Every give-up on a guest marker needs the record immediately before it: the
# marker phrase, a raise of the kind a proof exits with - single-line or spread
# over the argument list - and no harness socket in the same message.  The
# window is thirteen lines, which is what a record and the raise it belongs to
# occupy together.
undeclared_sites() {
    awk -v record="$RECORD" '
        BEGIN {
            size = 13
            for (slot = 0; slot < size; slot++) {
                is_record[slot] = 0
                is_raise[slot] = 0
            }
        }
        {
            slot = NR % size
            is_record[slot] = index($0, record) > 0
            is_raise[slot] = ($0 ~ /raise (SystemExit|TimeoutError)\(/)
            if ($0 !~ /timed out waiting/) { next }
            if ($0 ~ /monitor socket|QEMU monitor|monitor closed|serial socket/) {
                next
            }
            declared = 0
            raised = 0
            for (back = 0; back < size; back++) {
                if (NR - back < 1) { break }
                slot = (NR - back) % size
                if (is_record[slot]) { declared = 1 }
                if (is_raise[slot]) { raised = 1 }
            }
            if (raised && !declared) { print FILENAME ":" FNR ": " $0 }
        }
    ' "$@"
}

check() {
    local -a scripts=("$@")
    local status=0
    local script target seen=0

    if (( ${#scripts[@]} == 0 )); then
        while IFS=$'\t' read -r target script; do
            scripts+=("$script")
        done < <(pairs)
        if (( ${#scripts[@]} == 0 )); then
            fail 'the acceptance phase runs no target this reader can resolve'
            return 1
        fi
    fi

    local -A unique=()
    for script in "${scripts[@]}"; do
        [[ -f $root/$script || -f $script ]] || {
            fail "missing proof script: $script"
            status=1
            continue
        }
        local path=$script
        [[ -f $path ]] || path=$root/$script
        unique[$path]=1
    done

    for path in "${!unique[@]}"; do
        seen=$((seen + 1))
        # A proof that never gives up on a guest marker owes no declaration:
        # what the rule covers is the give-up, not the file.
        local sites
        sites=$(undeclared_sites "$path")
        if [[ -n $sites ]]; then
            fail "$path gives up on a guest marker without declaring it:"
            printf '%s\n' "$sites" >&2
            status=1
        fi
    done
    if (( seen == 0 )); then
        fail 'no proof script to check'
        return 1
    fi
    return "$status"
}

case ${1:-} in
    list)
        pairs
        ;;
    check)
        shift
        check "$@"
        ;;
    *)
        echo "usage: ${0##*/} list|check [script...]" >&2
        exit 2
        ;;
esac
