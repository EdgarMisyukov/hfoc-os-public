#!/usr/bin/env bash
# ITEM-661: derive the framebuffer title from VERSION and check what is drawn.

grep -Fq 'return "HFOC OS " HFOC_VERSION_STRING;' kernel/src/hfoc_build.c || {
    echo 'ITEM-661: заголовок не использует версию сборки' >&2
    exit 1
}
grep -Fq 'framebuffer_text(18, 12, hfoc_version_title(),' kernel/src/console.c || {
    echo 'ITEM-661: консоль не рисует заголовок с версией сборки' >&2
    exit 1
}
grep -Fq 'strcmp(header_capture, expected_header) == 0' tests/console-host.c || {
    echo 'ITEM-661: host-тест не сверяет нарисованный заголовок' >&2
    exit 1
}
