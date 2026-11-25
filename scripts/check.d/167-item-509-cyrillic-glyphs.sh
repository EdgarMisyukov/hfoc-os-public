#!/usr/bin/env bash
# ITEM-509 increment 3: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 3 makes the console print Russian.  Until this increment
# every byte of a Russian string reached the renderer on its own - two cells for
# one letter - and both cells were empty, because the font carried Latin only
# and `glyph_rows()` ended in a silent `default: break;`.  The claims pinned
# here are what the screen shows after the item:
#
#   * the font carries the Cyrillic alphabet in the same 5x7 cell as the Latin
#     tables, which are untouched, and the ASCII range still resolves through
#     the byte table;
#   * the console paints code points, not bytes: the string decoder is the
#     language catalog's own strict reader, wrapping and the one-cell advance
#     count code points, and the serial stream keeps the original bytes;
#   * a code point with no glyph - or a byte that cannot start or continue a
#     sequence - is a visible marker, a counter and a named record, never
#     silence;
#   * the deferred batch holds whole code points, so a flush cannot cut one in
#     half and replay as a marker plus a stray byte;
#   * the coverage proof walks the catalog's own inventory, so a Russian string
#     added to the catalog without its glyph fails by code point;
#   * both host proofs stay registered in the canonical host suite, where a
#     suite that loses one fails.
for item509_inc3_file in \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    kernel/src/console.c \
    kernel/include/appliance_language.h \
    tests/framebuffer-glyph-host.c \
    tests/console-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc3_file ]] || {
        echo "ITEM-509 inc3: отсутствует $item509_inc3_file" >&2
        exit 1
    }
done

# 1. The Cyrillic half of the font, and the Latin half untouched.  The ASCII
#    lookup lines are pinned by the ITEM-444 slice as well; they are repeated
#    here because this item is the one that could have moved them.
# ITEM-620 moved the glyphs into one Debian Fixed 8x16 table.  The row
# pictures are still present for every script the item promised, and the
# host proof below walks the catalog inventory.
for item509_inc3_table in \
    'static const struct font8x16_glyph font8x16_glyphs[] = {' \
    '{ 0x0410U,' \
    '{ 0x0430U,' \
    '{ 0x0401U,' \
    '{ 0x0451U,' \
    '{ 0x00ABU,' \
    '{ 0x00BBU,' \
    '{ 0x2014U,' \
    '{ 0x2026U,' \
    '{ 0x2116U,'; do
    grep -Fq "$item509_inc3_table" kernel/src/framebuffer.c || {
        echo "ITEM-509 inc3: шрифт потерял $item509_inc3_table" >&2
        exit 1
    }
done
grep -Fq 'if (codepoint < 0x80U) {' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc3: ASCII больше не делегируется в байтовую таблицу" >&2
    exit 1
}
grep -Fq 'glyph = font8x16_find(codepoint);' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc3: ASCII-ветка потеряла поиск глифа" >&2
    exit 1
}
grep -Fq 'return ascii_glyph_present((unsigned char)codepoint);' \
    kernel/src/framebuffer.c || {
    echo "ITEM-509 inc3: предикат ASCII не спрашивается для ASCII" >&2
    exit 1
}

# 2. The code-point layer and the marker that replaced the silence.
for item509_inc3_api in \
    'bool framebuffer_glyph_rows(uint32_t codepoint, uint8_t rows[16]);' \
    'bool framebuffer_char_codepoint(uint64_t x, uint64_t y, uint32_t codepoint,' \
    'uint64_t framebuffer_glyph_missing_count(void);' \
    'uint32_t framebuffer_glyph_missing_last(void);'; do
    grep -Fq -- "$item509_inc3_api" kernel/include/framebuffer.h || {
        echo "ITEM-509 inc3: framebuffer.h не объявляет: $item509_inc3_api" >&2
        exit 1
    }
done
for item509_inc3_marker in \
    'static const uint8_t glyph_missing_marker[16] =' \
    '++glyph_missing_count;' \
    'glyph_missing_last = codepoint;' \
    'rows[row] = glyph_missing_marker[row];'; do
    grep -Fq -- "$item509_inc3_marker" kernel/src/framebuffer.c || {
        echo "ITEM-509 inc3: маркер отсутствующего глифа не полон:" \
            "$item509_inc3_marker" >&2
        exit 1
    }
done
# The defect itself must be gone from the code-point path, not shadowed: the
# byte table's switch still ends in `default: break;` (its contract is the byte
# path), but the code-point lookup has to answer `false` so the caller marks it.
grep -Fq 'return false;' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc3: кодпойнт без глифа не отвергается" >&2
    exit 1
}

# 3. The console decodes whole strings with the catalog's reader, counts code
#    points for the wrap, keeps the serial bytes, and names what it refused.
for item509_inc3_console in \
    '#include "appliance_language.h"' \
    'appliance_language_utf8_next(text, length, &next,' \
    'static void console_emit_codepoint(uint32_t codepoint, const char *bytes,' \
    'static bool console_defer_push_bytes(const char *bytes, size_t length) {' \
    'static void console_emit_invalid_byte(unsigned char value)' \
    'console_emit_codepoint(0xFFFDU,' \
    '" byte=0x"' \
    '"[GLYPH-MISSING] codepoint=U+"'; do
    grep -Fq -- "$item509_inc3_console" kernel/src/console.c || {
        echo "ITEM-509 inc3: консоль не несёт: $item509_inc3_console" >&2
        exit 1
    }
done
# The replay decodes the batch, so a deferred code point is painted as itself.
grep -Fq 'appliance_language_utf8_next(console_defer_buffer,' \
    kernel/src/console.c || {
    echo "ITEM-509 inc3: replay не разбирает отложенную пачку" >&2
    exit 1
}
# The record must be published where the cell is painted - the live paint and
# the replay of a deferred one - not at the push, where the count is not yet
# the one the screen shows.
grep -Fq 'console_publish_glyph_missing(codepoint, invalid_byte);' \
    kernel/src/console.c || {
    echo "ITEM-509 inc3: запись об отсутствующем глифе не публикуется" >&2
    exit 1
}

# 4. The coverage gate: the proof asks the catalog for its inventory and walks
#    it, so the two cannot drift apart.  The console proof drives the real
#    console and requires one cell per code point, the same wrap as Latin, and
#    the named record.
grep -Fq 'appliance_language_ru_codepoints(catalog,' \
    tests/framebuffer-glyph-host.c || {
    echo "ITEM-509 inc3: проба покрытия не берёт инвентарь каталога" >&2
    exit 1
}
for item509_inc3_glyph_proof in \
    "the catalog's Russian code points fit" \
    'U+%04X has a glyph' \
    'a code point with no glyph still paints a visible cell' \
    'the missing glyph is counted' \
    'the last missing glyph is named' \
    'Ё is not drawn as Е' \
    'a Cyrillic letter is painted in both cases'; do
    grep -Fq -- "$item509_inc3_glyph_proof" \
        tests/framebuffer-glyph-host.c || {
        echo "ITEM-509 inc3: проба глифов не требует:" \
            "$item509_inc3_glyph_proof" >&2
        exit 1
    }
done

for item509_inc3_console_proof in \
    'six Russian letters are six cells, not twelve' \
    'a Cyrillic line wraps by code points, not by bytes' \
    'the serial transcript keeps the same bytes it always did' \
    '[GLYPH-MISSING] codepoint=U+4E00 count=1' \
    '[GLYPH-MISSING] codepoint=U+FFFD byte=0xD0 count=2'; do
    grep -Fq -- "$item509_inc3_console_proof" tests/console-host.c || {
        echo "ITEM-509 inc3: консольная проба не требует:" \
            "$item509_inc3_console_proof" >&2
        exit 1
    }
done

# 5. Both proofs stay in the canonical host suite: a Makefile target, the
#    language catalog linked into the glyph proof it now reads, and the
#    manifest entry the runner's list is built from.
for item509_inc3_registration in \
    'test-framebuffer-glyph-host:' \
    'test-console-host:' \
    '$(MAKE) test-framebuffer-glyph-host' \
    '$(MAKE) test-console-host'; do
    grep -Fq -- "$item509_inc3_registration" Makefile || {
        echo "ITEM-509 inc3: Makefile не регистрирует:" \
            "$item509_inc3_registration" >&2
        exit 1
    }
done
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc3: каталог не слинкован в пробы" >&2
    exit 1
}
grep -Fq 'test-framebuffer-glyph-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-509 inc3: манифест host suite не содержит" \
        "test-framebuffer-glyph-host" >&2
    exit 1
}
# The console proof is part of the canonical suite as well: the manifest pins
# it by name, so a suite that loses it fails there too.
grep -Fq 'test-console-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-509 inc3: манифест host suite не пинит test-console-host" >&2
    exit 1
}

# 6. The item is recorded where a maintainer looks: the console document owns
#    what the screen shows, the surface inventory owns the increment order, and
#    the queue and the roadmap own the item itself.
for item509_inc3_doc in \
    'ITEM-509 increment 3' \
    'cyrillic' \
    'appliance_language_utf8_next'; do
    grep -Fq -- "$item509_inc3_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует:" \
            "$item509_inc3_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-509 increment 3' docs/hfoc-language-surfaces.md || {
    echo "docs/hfoc-language-surfaces.md не фиксирует инкремент 3" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 3' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует инкремент 3" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 3' docs/ARCHITECTURE.md || {
    echo "docs/ARCHITECTURE.md не фиксирует инкремент 3" >&2
    exit 1
}
grep -Fq 'ITEM-509 increment 3' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-509 increment 3" >&2
    exit 1
}
