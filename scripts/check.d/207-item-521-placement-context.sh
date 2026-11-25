#!/usr/bin/env bash
# ITEM-521 (increment 7): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521pc_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521pc_root" || exit 1

# ITEM-521 increment 7: the placement seam receives the proven medium.
#
# The owner's ITEM-520 fork asks *where* the install journal goes - (a) a FAT12
# file created at runtime, (b) a raw extent at the end of the medium, (c) a
# file preallocated at build time - and every one of the three answers needs
# the same two things before it can write a byte: the block view of the medium
# the loader booted from, and the identity guard's own verdict for it (extent,
# partition, disk id, and the proof that it is that medium).  Before this
# increment the seam was declared `install_journal_platform_bind_medium(void)`,
# so no answer could reach a device at all and the increment that follows the
# owner's answer would have had to change the seam contract first.
#
# This increment hands the context over and chooses nothing:
#   * the guard's MATCH branch is still the only call site, and it passes
#     `&storage->block.device` - the block layer the medium was proven through -
#     and the guard's `&verdict`, so no backend has to look a device up again or
#     repeat an identity check;
#   * the seam receives both and still binds nothing: it names no placement
#     module, reads and writes no sector and returns false, exactly as slice 205
#     pins, so no byte of any medium is written and the install path is
#     unchanged;
#   * the fork itself stays the owner's: this slice deliberately pins no
#     placement token in the platform code.

# 1. The header declares the seam with the proven medium's two pieces.
for item521pc_file in \
    kernel/include/install_journal_platform.h \
    kernel/src/install_journal_platform.c \
    kernel/src/xhci.c \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md \
    docs/ROADMAP.md; do
    [[ -f $item521pc_file ]] || {
        echo "ITEM-521: отсутствует $item521pc_file" >&2
        exit 1
    }
done
item521pc_decl=$(awk '
    /^bool install_journal_platform_bind_medium\(/ { inside = 1 }
    inside { print }
    inside && /\);/ { exit }
' kernel/include/install_journal_platform.h)
[[ -n $item521pc_decl ]] || {
    echo "ITEM-521: заголовок не объявляет шов размещения" >&2
    exit 1
}
[[ $item521pc_decl == *'struct block_device *device'* ]] || {
    echo "ITEM-521: шов не принимает block-вид носителя: $item521pc_decl" >&2
    exit 1
}
[[ $item521pc_decl == *'const struct install_medium_verdict *verdict'* ]] || {
    echo "ITEM-521: шов не принимает вердикт стража носителя: $item521pc_decl" >&2
    exit 1
}
grep -Fq 'the boot-medium identity guard said MATCH' \
    kernel/include/install_journal_platform.h || {
    echo "ITEM-521: шов не называет, кто доказал носитель" >&2
    exit 1
}
grep -Fq 'valid only for the duration of the call' \
    kernel/include/install_journal_platform.h || {
    echo "ITEM-521: шов не называет время жизни вердикта" >&2
    exit 1
}

# 2. The definition receives the same two pieces.
item521pc_def=$(awk '
    /^bool install_journal_platform_bind_medium\(/ { inside = 1 }
    inside { print }
    inside && /\) \{/ { exit }
' kernel/src/install_journal_platform.c)
[[ -n $item521pc_def ]] || {
    echo "ITEM-521: определение шва не найдено" >&2
    exit 1
}
[[ $item521pc_def == *'struct block_device *device'* &&
   $item521pc_def == *'const struct install_medium_verdict *verdict'* ]] || {
    echo "ITEM-521: определение шва не принимает контекст: $item521pc_def" >&2
    exit 1
}

# 3. The one call site hands over the device the guard proved and its verdict.
[[ $(grep -c 'install_journal_platform_bind_medium(' kernel/src/xhci.c) -eq 1 ]] || {
    echo "ITEM-521: вызов шва не единственный в xhci.c" >&2
    exit 1
}
item521pc_call=$(awk '
    /if \(!install_journal_platform_bind_medium\(/ { inside = 1 }
    inside { print }
    inside && /\{/ { exit }
' kernel/src/xhci.c)
[[ -n $item521pc_call ]] || {
    echo "ITEM-521: вызов шва в xhci.c не найден" >&2
    exit 1
}
[[ $item521pc_call == *'&storage->block.device'* ]] || {
    echo "ITEM-521: вызов шва не передаёт block-вид носителя: $item521pc_call" >&2
    exit 1
}
[[ $item521pc_call == *'&verdict'* ]] || {
    echo "ITEM-521: вызов шва не передаёт вердикт стража: $item521pc_call" >&2
    exit 1
}
grep -Fq 'ITEM-520' kernel/src/xhci.c || {
    echo "ITEM-521: точка привязки не называет открытую развилку" >&2
    exit 1
}

# 4. The queue and the roadmap record this increment, and the queue still sends
#    the placement itself to the owner's answer.  The current-item pin follows
#    the item the queue is on (ITEM-564 moved it on from ITEM-547); this slice's
#    own record is its increment 7 section above and its roadmap entry below.
grep -A 3 '^## Item ITEM-547$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-547' || {
    echo "ITEM-521: очередь потеряла исторический раздел ITEM-547" >&2
    exit 1
}
grep -Fq 'ITEM-520' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-521: очередь не называет развилку владельца" >&2
    exit 1
}
grep -Fq 'инкремент 7' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет инкремент" >&2
    exit 1
}
