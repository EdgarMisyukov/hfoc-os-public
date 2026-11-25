#!/usr/bin/env bash
# ITEM-444: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The screen draws lowercase letters as lowercase.
#
# The 2026-09-18 hardware run typed `help` at the machine and the command ran,
# and `1234567890` arrived as digits, but every letter on the screen - in the
# boot records and in the echo of what was typed - was a capital.  The input
# path was already right; the renderer folded the case away:
#
#   if (character >= 'a' && character <= 'z') {
#       character = (char)(character - 'a' + 'A');
#   }
#
# The font carried 26 capital glyphs and nothing else, so glyph_rows() could
# only look a lowercase letter up as its capital, and no path - record or echo -
# could paint a lowercase letter at all.  The claims pinned here are what the
# screen shows after the item:
#
#   * the font carries 26 lowercase glyphs in the same 5x7 cell as the capitals,
#     and glyph_rows() selects them by the letter instead of folding the case;
#   * the capital and digit tables are untouched, so upper case and the records
#     that use it are unchanged;
#   * a host proof drives the real renderer - framebuffer.c itself, pointed at a
#     host surface - and requires every lowercase letter to differ from its
#     capital, the lowercase shapes to be distinct from each other, and a whole
#     string to keep its case on the way to the surface;
#   * a slice cannot read a machine's screen, so the host proof is the evidence
#     and this slice keeps it registered in the canonical host suite, where a
#     suite that loses it fails.
for framebuffer_lowercase_file in \
    kernel/src/framebuffer.c \
    tests/framebuffer-glyph-host.c \
    Makefile; do
    [[ -f $framebuffer_lowercase_file ]] || {
        echo "ITEM-444: отсутствует $framebuffer_lowercase_file" >&2
        exit 1
    }
done

# ITEM-620 replaced the five separate 5x7 tables with one Debian Fixed 8x16
# table.  The lowercase letters are still their own pictures in it, and the
# lookup still takes the letter itself (its code point), never a folded case.
grep -Fq 'static const struct font8x16_glyph font8x16_glyphs[] = {' \
    kernel/src/framebuffer.c || {
    echo "ITEM-444: шрифт не несёт таблицы глифов" >&2
    exit 1
}
grep -Fq 'font8x16_find((unsigned char)character)' \
    kernel/src/framebuffer.c || {
    echo "ITEM-444: строчная буква не выбирает свой глиф" >&2
    exit 1
}
# The defect itself must be gone, not shadowed: a renderer that still folds the
# case would paint capitals even with the table present.
if grep -Fq "character = (char)(character - 'a' + 'A');" \
    kernel/src/framebuffer.c; then
    echo "ITEM-444: рендерер всё ещё сворачивает регистр" >&2
    exit 1
fi
# Upper case and the digits keep their pictures in the same table, and the
# host proof below still requires each painted letter and digit.
grep -Fq '{ 0x0041U,' kernel/src/framebuffer.c || {
    echo "ITEM-444: таблица прописных глифов потеряна" >&2
    exit 1
}
grep -Fq '{ 0x0030U,' kernel/src/framebuffer.c || {
    echo "ITEM-444: таблица цифр потеряна" >&2
    exit 1
}

# The host proof renders through framebuffer_char(), compares the two cases of
# every letter and keeps the string-level case check, so the evidence is the
# painted cell and not a copy of the table.
for framebuffer_lowercase_proof in \
    'framebuffer_char(0U, 0U, value, 0xFFFFFFFFU, 1U);' \
    "lower case '%c' differs from upper case '%c'" \
    "lower case '%c' differs from lower case '%c'" \
    'a string keeps its case on the way to the framebuffer' \
    'the echoed command keeps its case'; do
    grep -Fq -- "$framebuffer_lowercase_proof" \
        tests/framebuffer-glyph-host.c || {
        echo "ITEM-444: host-проба не требует:" \
            "$framebuffer_lowercase_proof" >&2
        exit 1
    }
done

# The proof is part of the canonical host suite, both as a Makefile target and
# on the runner's list: a drill nobody runs is not evidence.
# shellcheck disable=SC2016  # the Makefile text is what the search must match
for framebuffer_lowercase_registration in \
    '$(MAKE) test-framebuffer-glyph-host' \
    'test-framebuffer-glyph-host:'; do
    grep -Fq -- "$framebuffer_lowercase_registration" Makefile || {
        echo "ITEM-444: host-проба не зарегистрирована в Makefile:" \
            "$framebuffer_lowercase_registration" >&2
        exit 1
    }
done
grep -Fq 'test-framebuffer-glyph-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-444: манифест host suite не содержит test-framebuffer-glyph-host" >&2
    exit 1
}

# The item is recorded where a maintainer looks: the console document owns what
# the screen shows, and the queue owns the item itself.
for framebuffer_lowercase_doc in \
    'ITEM-444' \
    'framebuffer-glyph-host'; do
    grep -Fq -- "$framebuffer_lowercase_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-444:" \
            "$framebuffer_lowercase_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-444' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-444" >&2
    exit 1
}
grep -Fq 'ITEM-444' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-444" >&2
    exit 1
}
