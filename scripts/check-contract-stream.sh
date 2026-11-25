#!/usr/bin/env bash
# ITEM-387: print the ordered stream of enforced statements of the check suite.
#
# The suite lives in scripts/check.sh, which keeps only the entry-point contract
# and sources the contract modules under scripts/check.d/ in a pinned order. This
# tool flattens that structure back into the sequence of statements the suite
# actually enforces, in execution order:
#
#   * shebang, `set` flags, locale exports, ROOT/cd bootstrap, comments and blank
#     lines are structure, not contracts, and are not part of the stream;
#   * the pinned module list is not a statement: the modules are inlined in the
#     order the list pins, exactly as the entry point sources them;
#   * every other line is a contract and is emitted verbatim (trimmed), so a
#     deleted, weakened, duplicated or reordered statement changes the stream.
#
# scripts/verify-check-contracts.sh compares this stream against the frozen
# pre-split baseline; this tool is the extraction half of that proof.
#
# Usage: check-contract-stream.sh <path-to-check.sh>
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

check_file=${1:?usage: check-contract-stream.sh <path-to-check.sh>}
[[ -f "$check_file" ]] || {
    echo "check-contract-stream: no such file: $check_file" >&2
    exit 1
}
module_dir=$(cd -- "$(dirname -- "$check_file")" && pwd -P)/check.d

fail() {
    echo "check-contract-stream: $*" >&2
    exit 1
}

# Structure lines of the entry point and of every module. Everything that does
# not match is a contract statement and is emitted.
is_structure() {
    local line=$1
    [[ -z ${line//[[:space:]]/} ]] && return 0
    [[ $line =~ ^[[:space:]]*# ]] && return 0
    [[ $line == '#!/usr/bin/env bash' ]] && return 0
    [[ $line == 'set -Eeuo pipefail' ]] && return 0
    [[ $line == 'export LC_ALL=C' ]] && return 0
    [[ $line == 'export LANG=C' ]] && return 0
    # shellcheck disable=SC2016  # the compared text is literal, not expanded
    [[ $line == 'ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)' ]] && return 0
    # shellcheck disable=SC2016  # the compared text is literal, not expanded
    [[ $line == 'cd "$ROOT"' ]] && return 0
    return 1
}

# Emit the stream of one file. Modules are inlined where the entry point sources
# them, so the output is one flat, ordered contract sequence.
stream_file() {
    local file=$1
    local -a modules=()
    local line
    local module
    local in_array=0
    local in_loop=0
    local loop_sources=0
    local saw_array=0
    local saw_loop=0
    local seen_modules=' '

    [[ -f $file ]] || fail "no such file: $file"
    while IFS= read -r line || [[ -n $line ]]; do
        trimmed=${line#"${line%%[![:space:]]*}"}
        trimmed=${trimmed%"${trimmed##*[![:space:]]}"}
        if ((in_array)); then
            if [[ $trimmed == ')' ]]; then
                in_array=0
                continue
            fi
            [[ $trimmed =~ ^([A-Za-z0-9._-]+\.sh)$ ]] ||
                fail "unexpected line in CHECK_MODULES: $line"
            module=${BASH_REMATCH[1]}
            [[ $seen_modules == *" $module "* ]] && fail "module listed twice: $module"
            seen_modules+="$module "
            modules+=("$module")
            continue
        fi
        if ((in_loop)); then
            if [[ $trimmed == 'done' ]]; then
                in_loop=0
                ((loop_sources == 1)) ||
                    fail "the module loop must source exactly once, found $loop_sources"
                continue
            fi
            if is_structure "$trimmed"; then
                continue
            fi
            # shellcheck disable=SC2016  # the compared text is literal
            [[ $trimmed == 'source "$ROOT/scripts/check.d/$check_module"' ]] ||
                fail "unexpected line in the module loop: $line"
            loop_sources=$((loop_sources + 1))
            continue
        fi
        if [[ $trimmed == 'CHECK_MODULES=(' ]]; then
            ((saw_array == 0)) || fail "CHECK_MODULES declared twice in $file"
            saw_array=1
            in_array=1
            continue
        fi
        # shellcheck disable=SC2016  # the compared text is literal
        if [[ $trimmed == 'for check_module in "${CHECK_MODULES[@]}"; do' ]]; then
            ((saw_loop == 0)) || fail "module loop declared twice in $file"
            saw_loop=1
            in_loop=1
            loop_sources=0
            continue
        fi
        is_structure "$trimmed" && continue
        printf '%s\n' "$trimmed"
    done <"$file"
    ((in_array == 0)) || fail "unterminated CHECK_MODULES in $file"
    ((in_loop == 0)) || fail "unterminated module loop in $file"
    ((saw_array == saw_loop)) ||
        fail "CHECK_MODULES and its source loop must appear together in $file"
    if ((saw_array)); then
        [[ -d $module_dir ]] || fail "missing module directory: $module_dir"
        local -a on_disk=()
        local path
        for path in "$module_dir"/*.sh; do
            [[ -e $path ]] || fail "no contract modules in $module_dir"
            on_disk+=("${path##*/}")
        done
        local listed sorted_listed sorted_disk
        listed=$(printf '%s\n' "${modules[@]}" | sort)
        sorted_listed=$listed
        sorted_disk=$(printf '%s\n' "${on_disk[@]}" | sort)
        [[ $sorted_listed == "$sorted_disk" ]] || {
            echo "check-contract-stream: pinned module list and $module_dir disagree" >&2
            diff <(printf '%s\n' "$sorted_listed") <(printf '%s\n' "$sorted_disk") >&2 || true
            exit 1
        }
        for module in "${modules[@]}"; do
            [[ -f $module_dir/$module ]] || fail "listed module is missing: $module"
            stream_file "$module_dir/$module"
        done
    fi
}

stream_file "$check_file"
