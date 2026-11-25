#!/usr/bin/env bash
# ITEM-565: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The queue's `## Current item` heading moves on every time an item is merged.
# An item slice that pins that heading therefore turns a historical contract
# into a dependency on whatever the queue is on today: when ITEM-564's slice
# pinned it, advancing to ITEM-565 would have broken the older slice, and the
# queue stayed parked on an item that had already been merged twice.  This
# slice drives the real queue check against fixtures and requires it to name a
# current item ahead of the history and to reject a module that pins the moving
# heading.

ITEM565_CHECK=scripts/check.d/52-queue-current-item.sh
[[ -f $ITEM565_CHECK ]] || {
    echo "ITEM-565: отсутствует $ITEM565_CHECK" >&2
    exit 1
}

item565_tmp=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-item565.XXXXXX")
trap 'rm -rf "$item565_tmp"' EXIT

item565_history() {
    local last=$1
    local number
    for number in $(seq 550 "$last"); do
        printf '## Item ITEM-%s\n\nFixture history %s.\n\n' "$number" "$number"
    done
}

# make_queue <file> <current> <history-last> [extra-heading]
item565_make_queue() {
    local file=$1
    local current=$2
    local history_last=$3
    local extra=${4:-}

    item565_history "$history_last" >"$file"
    {
        printf '## Current item\n\n**ITEM-%s — fixture current item.**\n\nFixture body.\n' \
            "$current"
        if [[ -n $extra ]]; then
            printf '\n%s\n' "$extra"
        fi
    } >>"$file"
}

item565_run() {
    local queue=$1
    local modules=$2

    HFOC_QUEUE_DOC=$queue HFOC_QUEUE_CHECK_DIR=$modules \
        bash -c 'source "$1"' _ "$ITEM565_CHECK" >/dev/null 2>&1
}

item565_expect() {
    local expected=$1
    local name=$2
    local queue=$3
    local modules=$4

    if item565_run "$queue" "$modules"; then
        actual=0
    else
        actual=1
    fi
    [[ $actual == "$expected" ]] || {
        echo "ITEM-565: fixture '$name' returned $actual, expected $expected" >&2
        exit 1
    }
}

mkdir -p "$item565_tmp/modules" "$item565_tmp/bad-modules"
printf '#!/usr/bin/env bash\n# no current-item pin here\n' \
    >"$item565_tmp/modules/10-clean.sh"

# The bad module is the exact shape ITEM-564's slice had: it takes the next
# item's number from the heading that moves.
cat >"$item565_tmp/bad-modules/99-pins-current.sh" <<'EOF'
#!/usr/bin/env bash
grep -A 3 '^## Current item$' "$QUEUE_DOC" | grep -Fq 'ITEM-'
EOF

# 1. A current item ahead of the history, and no module pinning the moving
#    heading, is the healthy queue.
item565_make_queue "$item565_tmp/valid.md" 560 559
item565_expect 0 valid "$item565_tmp/valid.md" "$item565_tmp/modules"

# 2. The current item must be ahead of every historical item: a queue parked
#    on a merged (or older) number is exactly the state the stale pin produced.
item565_make_queue "$item565_tmp/behind.md" 555 559
item565_expect 1 behind "$item565_tmp/behind.md" "$item565_tmp/modules"

# 3. The current item's number must not also be a historical section.
item565_make_queue "$item565_tmp/repeated.md" 561 561
item565_expect 1 repeated "$item565_tmp/repeated.md" "$item565_tmp/modules"

# 4. Exactly one current heading, and it is the last section.
item565_make_queue "$item565_tmp/twice.md" 560 559 '## Current item'
item565_expect 1 twice "$item565_tmp/twice.md" "$item565_tmp/modules"

item565_make_queue "$item565_tmp/not-last.md" 560 559 '## Item ITEM-561'
item565_expect 1 not-last "$item565_tmp/not-last.md" "$item565_tmp/modules"

# 5. The heading must name an item number.
item565_history 559 >"$item565_tmp/unnamed.md"
{
    printf '## Current item\n\n**The next item.**\n'
} >>"$item565_tmp/unnamed.md"
item565_expect 1 unnamed "$item565_tmp/unnamed.md" "$item565_tmp/modules"

# 6. A module that greps the moving heading is the defect this item removes;
#    only the queue check itself may read it.
item565_expect 1 pinned-module "$item565_tmp/valid.md" "$item565_tmp/bad-modules"

echo "ITEM-565: queue current-item fixtures passed"
