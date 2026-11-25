#!/usr/bin/env bash
# ITEM-521 (increment 8): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521un_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521un_root" || exit 1

# ITEM-521 increment 8: the proven medium that no placement was bound to is
# named, not silently staged.
#
# The owner's requirement 3 asks for more than "a journal that cannot be
# written does not fail the run": the write about it has to be an ordinary
# record.  Increment 6 named the medium that cannot carry a journal at all (an
# optical boot).  This increment names the other half: the identity guard
# proved the medium, the placement seam bound nothing on it, and the records
# the journal staged in its bounded ring will therefore never reach the medium.
#
# That state is not an accident of the answered fork - it is what every refusal
# of every answer to ITEM-520 leaves behind (a medium whose build reserved no
# file, a reserved chain the FAT does not back, a raw extent that overlaps a
# partition all leave the journal unbound).
# So the naming is placed where the answer cannot forget it and where it is
# host-testable:
#   * `install_journal_platform_medium_unbound()` is the platform module's own
#     one-line policy - the core's `install_journal_refuse()` with the reason
#     `NO_PLACEMENT` - so the reason string and the once-only rule live with
#     the journal's other contact points and not in a driver;
#   * the naming is the *false* branch of the seam call in the guard's MATCH
#     branch, before the probe returns: a placement that refused for a better
#     reason (`NOT_FAT12`, `GEOMETRY`, `SIGNATURE`) has already named it, and
#     the core keeps the first name, so a specific reason is never overwritten
#     by this fallback;
#   * the seam's body binds the chosen placement or refuses by name (slices 205
#     and 207 pin that and the context), and this slice's witness proves on the
#     host with a RAM medium that a refused bind writes no sector and that the
#     fallback never overwrites the specific name;
#   * nothing fails the run: the naming returns false like every refusal, and
#     the refusal page's flush and the panic path stay silent when a loss was
#     already named, so the boot-time shape stays exactly one line.
#
# The placement itself is the owner's answer - the reserved FAT12 file - so
# this slice pins the naming policy, not the placement token.

# 1. The module, the seam, the call site, the witness and the documents exist.
for item521un_file in \
    kernel/include/install_journal_platform.h \
    kernel/src/install_journal_platform.c \
    kernel/src/xhci.c \
    kernel/src/install_journal_core.c \
    tests/install-journal-platform-host.c \
    Makefile \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md \
    docs/ROADMAP.md; do
    [[ -f $item521un_file ]] || {
        echo "ITEM-521: отсутствует $item521un_file" >&2
        exit 1
    }
done

# 2. The header declares the naming, next to the other requirement 3 refusal,
#    and states the two rules the core enforces for it: the first name stands
#    and the run is never failed.
grep -Fq 'bool install_journal_platform_medium_unbound(void);' \
    kernel/include/install_journal_platform.h || {
    echo "ITEM-521: заголовок не объявляет имя непривязанного носителя" >&2
    exit 1
}
grep -Fq 'keeps that name' kernel/include/install_journal_platform.h || {
    echo "ITEM-521: заголовок не называет правило первого имени" >&2
    exit 1
}
grep -Fq 'the run is never failed' kernel/include/install_journal_platform.h || {
    echo "ITEM-521: заголовок не называет, что отказ не валит прогон" >&2
    exit 1
}

# 3. The definition is the core's own refusal with one reason: the platform
#    module does not grow a second state machine, and it still writes no status
#    text of its own (the core owns the format, slice 205 pins the negative).
item521un_def=$(awk '
    /^bool install_journal_platform_medium_unbound\(void\) \{/ { inside = 1 }
    inside { print }
    inside && /^\}/ { exit }
' kernel/src/install_journal_platform.c)
[[ -n $item521un_def ]] || {
    echo "ITEM-521: имя непривязанного носителя не определено" >&2
    exit 1
}
[[ $item521un_def == *'install_journal_refuse(&platform_journal, "NO_PLACEMENT")'* ]] || {
    echo "ITEM-521: имя непривязанного носителя не идёт через отказ ядра" >&2
    exit 1
}
if printf '%s\n' "$item521un_def" | grep -Fq '[INSTALL-LOG]'; then
    echo "ITEM-521: модуль платформы сам печатает формат журнала" >&2
    exit 1
fi

# 4. The call site: the naming is the false answer of the seam call in the
#    guard's MATCH branch, it is the only naming in xhci.c, and the seam's own
#    body binds the chosen placement or refuses by name (slice 205 pins the
#    body, 207 the context).
[[ $(grep -c 'install_journal_platform_medium_unbound(' kernel/src/xhci.c) -eq 1 ]] || {
    echo "ITEM-521: имя непривязанного носителя не названо ровно один раз" >&2
    exit 1
}
item521un_if=$(grep -n 'if (!install_journal_platform_bind_medium(&storage->block.device,' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
item521un_name=$(grep -n '(void)install_journal_platform_medium_unbound();' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
item521un_return=$(grep -n 'return verdict.result == INSTALL_MEDIUM_MATCH;' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
[[ -n $item521un_if && -n $item521un_name && -n $item521un_return ]] || {
    echo "ITEM-521: не найдены ложная ветка шва или имя потери" >&2
    exit 1
}
((item521un_if < item521un_name && item521un_name < item521un_return)) || {
    echo "ITEM-521: имя потери стоит не в ложной ветке шва" >&2
    exit 1
}

# 5. The host witness is registered in the host suite and carries the
#    assertions this increment is about: the named line, the route through the
#    store, the once-only rule, the refused bind that writes no sector and the
#    non-failing returns.  A witness that is not run proves nothing.
grep -Fq 'test-install-journal-platform-host' Makefile || {
    echo "ITEM-521: host-проба платформы не зарегистрирована" >&2
    exit 1
}
grep -Fq '	$(MAKE) test-install-journal-platform-host' Makefile || {
    echo "ITEM-521: host-проба платформы не входит в набор" >&2
    exit 1
}
for item521un_pin in \
    '[INSTALL-LOG] state=UNAVAILABLE reason=NO_PLACEMENT' \
    'a named FAT12 partition binds and writes only inside it' \
    'naming the unbound medium never fails the run' \
    'the first named reason stands and is not overwritten' \
    'the panic path does not re-name an already named loss' \
    'the optical reason stands after a later MATCH' \
    'a medium without FAT12 is refused and writes no sector' \
    'the signature refusal writes no sector'; do
    grep -Fq "$item521un_pin" tests/install-journal-platform-host.c || {
        echo "ITEM-521: host-проба платформы не проверяет: $item521un_pin" >&2
        exit 1
    }
done

# 6. The queue and the roadmap record this increment, and the queue still sends
#    the placement itself to the owner's answer.  The current-item pin follows
#    the item the queue is on (ITEM-564 moved it on from ITEM-547); this slice's
#    own record is its increment 8 section above and its roadmap entry below.
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
grep -Fq 'инкремент 8' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет инкремент" >&2
    exit 1
}
