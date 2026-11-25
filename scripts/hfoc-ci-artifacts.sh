#!/usr/bin/env bash
# ITEM-462: keep the CI diagnostics artifact small and useful.
#
# The GitHub Actions artifact quota for the whole account was exhausted by this
# repository's diagnostics uploads: 418 artifacts, 4.1 GiB, the largest being
# `hfoc-ci-diagnostics-<run_id>` at ~481 MiB each, while `build/test-artifacts/`
# holds 16 GiB of raw disk images and logs on the runner.  This tool is the
# single owner of the three rules that keep it bounded:
#
#   * `collect`  builds a bounded staging directory: text evidence only (logs,
#     summaries, `*.txt`, `*.sha256`), each file kept as a tail when it is large,
#     and a total byte budget.  Raw disk images, ISOs and other binaries never
#     enter it; each is named once in `EXCLUDED.txt` with its size.
#   * `cleanup`  deletes test-artifact directories older than N days from the
#     runner and publishes how much it removed.
#   * `guard`    rejects a workflow whose diagnostics upload would ship
#     `build/test-artifacts/**` unqualified, keep the retention unbounded, or
#     omit the collector and the cleanup.
#
# The host proof is tests/test-ci-artifacts-host.sh; the static slice is
# scripts/check.d/69-ci-artifacts.sh.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

usage() {
    cat >&2 <<'EOF'
usage:
  hfoc-ci-artifacts.sh cleanup --root DIR --days N
  hfoc-ci-artifacts.sh collect --root DIR --dest DIR --budget-bytes N
                                 [--since EPOCH] [--per-file-bytes N]
  hfoc-ci-artifacts.sh guard [--workflow FILE]

  cleanup  remove immediate subdirectories of --root older than --days and
           publish `[CI-ARTIFACTS] cleanup removed_dirs= removed_bytes=`.
  collect  copy bounded text evidence into --dest and publish
           `[CI-ARTIFACTS] collect files= bytes= excluded= truncated=`.
  guard    verify the diagnostics upload contract and publish
           `[CI-ARTIFACTS] guard state=OK|FAIL reason=`.
EOF
}

die_usage() {
    printf 'hfoc-ci-artifacts: %s\n' "$*" >&2
    usage
    exit 2
}

is_uint() {
    [[ ${1:-} =~ ^[0-9]+$ ]]
}

# The text evidence the artifact keeps.  Everything else is named in
# EXCLUDED.txt instead of being uploaded.
is_text_evidence() {
    local name=$1
    case $name in
        *.log | *.txt | *.sha256 | *.md | *.json | *.csv | *.summary) return 0 ;;
        manifest | manifest.* | *.manifest) return 0 ;;
        *) return 1 ;;
    esac
}

cmd_cleanup() {
    local root=
    local days=
    while (( $# )); do
        case $1 in
            --root)
                shift
                [[ $# -gt 0 ]] || die_usage '--root requires a directory'
                root=$1
                ;;
            --days)
                shift
                [[ $# -gt 0 ]] || die_usage '--days requires a number'
                days=$1
                ;;
            *) die_usage "unknown cleanup option: $1" ;;
        esac
        shift
    done
    [[ -n $root ]] || die_usage 'cleanup requires --root'
    is_uint "$days" || die_usage 'cleanup requires a non-negative --days'

    local removed_dirs=0
    local removed_bytes=0
    if [[ -d $root ]]; then
        while IFS= read -r -d '' dir; do
            local bytes
            bytes=$(du -sb -- "$dir" 2>/dev/null | awk '{print $1}')
            [[ $bytes =~ ^[0-9]+$ ]] || bytes=0
            rm -rf -- "$dir"
            removed_dirs=$((removed_dirs + 1))
            removed_bytes=$((removed_bytes + bytes))
        done < <(find "$root" -mindepth 1 -maxdepth 1 -type d \
            -mtime +"$days" -print0 2>/dev/null)
    fi
    printf '[CI-ARTIFACTS] cleanup root=%s days=%s removed_dirs=%d removed_bytes=%d\n' \
        "$root" "$days" "$removed_dirs" "$removed_bytes"
}

cmd_collect() {
    local root=
    local dest=
    local budget=
    local since=
    local per_file=262144
    while (( $# )); do
        case $1 in
            --root)
                shift
                [[ $# -gt 0 ]] || die_usage '--root requires a directory'
                root=$1
                ;;
            --dest)
                shift
                [[ $# -gt 0 ]] || die_usage '--dest requires a directory'
                dest=$1
                ;;
            --budget-bytes)
                shift
                [[ $# -gt 0 ]] || die_usage '--budget-bytes requires a number'
                budget=$1
                ;;
            --since)
                shift
                [[ $# -gt 0 ]] || die_usage '--since requires an epoch'
                since=$1
                ;;
            --per-file-bytes)
                shift
                [[ $# -gt 0 ]] || die_usage '--per-file-bytes requires a number'
                per_file=$1
                ;;
            *) die_usage "unknown collect option: $1" ;;
        esac
        shift
    done
    [[ -n $root ]] || die_usage 'collect requires --root'
    [[ -n $dest ]] || die_usage 'collect requires --dest'
    is_uint "$budget" || die_usage 'collect requires an integer --budget-bytes'
    is_uint "$per_file" || die_usage 'collect requires an integer --per-file-bytes'
    if [[ -n $since ]]; then
        is_uint "$since" || die_usage '--since must be an epoch in seconds'
    fi

    rm -rf -- "$dest"
    mkdir -p -- "$dest"

    local find_args=("$root" -type f)
    if [[ -n $since && $since != 0 ]]; then
        find_args+=(-newermt "@$since")
    fi

    local files=0
    local bytes=0
    local excluded=0
    local truncated=0
    local budget_exhausted=0
    local rel
    local out
    local size
    local take
    : >"$dest/EXCLUDED.txt"

    while IFS= read -r -d '' file; do
        rel=${file#"$root"/}
        size=$(stat -c '%s' -- "$file" 2>/dev/null || echo 0)
        [[ $size =~ ^[0-9]+$ ]] || size=0

        if ! is_text_evidence "${file##*/}"; then
            printf 'excluded %s size=%s reason=not-text-evidence\n' \
                "$rel" "$size" >>"$dest/EXCLUDED.txt"
            excluded=$((excluded + 1))
            continue
        fi

        if (( size == 0 )); then
            continue
        fi

        take=$size
        if (( take > per_file )); then
            take=$per_file
            truncated=$((truncated + 1))
        fi
        if (( bytes + take > budget )); then
            budget_exhausted=1
            break
        fi

        out="$dest/$rel"
        mkdir -p -- "${out%/*}"
        if (( take == size )); then
            cp -- "$file" "$out"
        else
            # The contract asks for the log's tail, not the whole file.
            tail -c "$take" -- "$file" >"$out"
            printf 'truncated %s original_bytes=%s kept_bytes=%s\n' \
                "$rel" "$size" "$take" >>"$dest/EXCLUDED.txt"
        fi
        files=$((files + 1))
        bytes=$((bytes + take))
    done < <(find "${find_args[@]}" -print0 2>/dev/null | sort -z)

    printf '[CI-ARTIFACTS] collect root=%s dest=%s files=%d bytes=%d excluded=%d truncated=%d budget_bytes=%d budget_exhausted=%d\n' \
        "$root" "$dest" "$files" "$bytes" "$excluded" "$truncated" \
        "$budget" "$budget_exhausted"
}

# Print the block of one `- name: <step>` step (the step's own keys and the
# lines beneath it, until the next step at the same indentation).
step_block() {
    local file=$1
    local needle=$2
    awk -v needle="      - name: $needle" '
        index($0, "      - name: ") == 1 { inside = (index($0, needle) == 1); next }
        inside { print }
    ' "$file"
}

cmd_guard() {
    local workflow=.github/workflows/ci.yml
    while (( $# )); do
        case $1 in
            --workflow)
                shift
                [[ $# -gt 0 ]] || die_usage '--workflow requires a file'
                workflow=$1
                ;;
            *) die_usage "unknown guard option: $1" ;;
        esac
        shift
    done

    guard_fail() {
        printf '[CI-ARTIFACTS] guard workflow=%s state=FAIL reason=%s\n' \
            "$workflow" "$1"
        exit 1
    }

    [[ -f $workflow ]] || guard_fail WORKFLOW_MISSING

    local upload
    upload=$(step_block "$workflow" 'Upload diagnostics')
    [[ -n $upload ]] || guard_fail UPLOAD_STEP_MISSING

    # An unqualified include of the raw test-artifact tree would ship the disk
    # images again.  A line that starts with `!` is an exclusion, not a path.
    if grep -vE '^[[:space:]]*#' <<<"$upload" |
        grep -Eq '^[[:space:]]+build/test-artifacts/'; then
        guard_fail TEST_ARTIFACTS_UNQUALIFIED
    fi
    # And the exclusions the contract names must be present.
    for required in '!**/*.raw' '!**/*.img' '!build/hfoc-os-*.iso'; do
        grep -Fq -- "$required" <<<"$upload" ||
            guard_fail "EXCLUSION_MISSING:${required}"
    done

    local retention
    retention=$(awk -F: '/retention-days:/ { gsub(/[^0-9]/, "", $2); print $2; exit }' \
        <<<"$upload")
    is_uint "$retention" || guard_fail RETENTION_MISSING
    (( retention <= 14 )) || guard_fail "RETENTION_TOO_LONG:${retention}"

    grep -Fq 'scripts/hfoc-ci-artifacts.sh collect' "$workflow" ||
        guard_fail COLLECT_STEP_MISSING
    grep -Fq -- '--budget-bytes' "$workflow" ||
        guard_fail COLLECT_BUDGET_MISSING
    grep -Fq 'scripts/hfoc-ci-artifacts.sh cleanup' "$workflow" ||
        guard_fail CLEANUP_STEP_MISSING
    grep -Fq -- '--days' "$workflow" || guard_fail CLEANUP_DAYS_MISSING

    printf '[CI-ARTIFACTS] guard workflow=%s state=OK retention_days=%s\n' \
        "$workflow" "$retention"
}

[[ $# -gt 0 ]] || die_usage 'a mode is required'
mode=$1
shift
case $mode in
    cleanup) cmd_cleanup "$@" ;;
    collect) cmd_collect "$@" ;;
    guard) cmd_guard "$@" ;;
    -h | --help | help) usage ;;
    *) die_usage "unknown mode: $mode" ;;
esac
