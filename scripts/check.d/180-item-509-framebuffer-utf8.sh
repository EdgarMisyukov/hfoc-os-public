#!/usr/bin/env bash
# ITEM-509 increment 17: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 17 (509.7c-1c): the framebuffer text path decodes UTF-8.
# The installer screens and the console's framebuffer backend hand
# `framebuffer_text()` the catalog's UTF-8 text; before this increment every
# byte became its own glyph, so a Russian screen would have printed mojibake the
# moment 509.7c-2 supplied a language.  ASCII stays byte for byte (the byte path
# is still `framebuffer_char`), a code point with no glyph draws the increment-3
# visible marker and is counted, and an invalid or truncated sequence costs one
# cell and does not swallow the rest of the line.
for item509_inc17_file in \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    tests/framebuffer-glyph-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc17_file ]] || {
        echo "ITEM-509 inc17: отсутствует $item509_inc17_file" >&2
        exit 1
    }
done

# 1. The text path decodes code points and draws them with the increment-3
#    tables; the old byte loop is gone.
grep -Fq 'appliance_language_utf8_next' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc17: путь текста не декодирует UTF-8" >&2
    exit 1
}
grep -Fq 'static void framebuffer_codepoint(' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc17: нет отрисовки по кодпойнту" >&2
    exit 1
}
grep -Fq 'glyph_rows_codepoint(codepoint, rows)' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc17: кодпойнт не ищется в таблицах глифов" >&2
    exit 1
}
grep -Fq 'glyph_missing_last = codepoint;' kernel/src/framebuffer.c || {
    echo "ITEM-509 inc17: кодпойнт без глифа не назван" >&2
    exit 1
}
if grep -Fq 'while (*text != ' kernel/src/framebuffer.c; then
    echo "ITEM-509 inc17: байтовый цикл текста остался" >&2
    exit 1
fi
# The live contract note in the header must not keep promising the byte path the
# increment replaced: callers read that comment, not the queue.
grep -Fq '`framebuffer_text()` decodes UTF-8 with the same catalog decoder' \
    kernel/include/framebuffer.h || {
    echo "ITEM-509 inc17: framebuffer.h всё ещё обещает побайтовый текст" >&2
    exit 1
}

# 2. The proof renders Russian through the text path, checks the ASCII cell, the
#    code-point advance, the visible marker and the truncated sequence.
for item509_inc17_check in \
    'a two-byte Russian letter is one cell with its own glyph' \
    'the ASCII path keeps painting the Latin cell it always painted' \
    'the cell advance counts code points, not bytes' \
    'a code point with no glyph draws the visible marker and is counted' \
    'a truncated sequence costs one cell and the line continues'; do
    grep -Fq -- "$item509_inc17_check" tests/framebuffer-glyph-host.c || {
        echo "ITEM-509 inc17: проба не проверяет: $item509_inc17_check" >&2
        exit 1
    }
done
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc17: проба не линкует каталог" >&2
    exit 1
}

# 3. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 17' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc17: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7c-1c' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc17: очередь не называет инкремент" >&2
    exit 1
}
