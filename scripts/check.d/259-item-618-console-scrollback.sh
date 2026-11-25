#!/usr/bin/env bash
# ITEM-618: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with shellcheck.
#
# The owner asked for a black console body and header, a light readable font at
# roughly the old size (10-15% smaller is acceptable) and a way to read the whole
# long `network`/`diag` answer.  This slice pins the in-tree bitmap metrics and
# palette, the bounded text ring, the non-blocking `scroll` viewer, the translated
# key hint, and the host/QEMU proofs.  It does not add an external font binary.
# shellcheck disable=SC2016,SC2041

item618_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item618_root" || exit 1

# 1. The body and header are black, the text is light, and the cell was 6x8 in 618 (8x16 from 620).
for item618_palette in \
    '#define BACKGROUND 0x000000' \
    '#define HEADER 0x000000' \
    '#define TEXT 0xf5f5f5'; do
    grep -Fq -- "$item618_palette" kernel/src/console.c || {
        echo "ITEM-618: палитра консоли не содержит:" "$item618_palette" >&2
        exit 1
    }
done
grep -Fq '#define FRAMEBUFFER_FONT_CELL_WIDTH 8U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-618: ширина клетки изменилась" >&2
    exit 1
}
grep -Fq '#define FRAMEBUFFER_FONT_CELL_HEIGHT 16U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-618: высота клетки не 16" >&2
    exit 1
}

# 2. The ring is bounded, excludes the input echo/pager repaint, and has the
#    `scroll` command with the real arrow/home/end events.
for item618_ring in \
    '#define SCROLLBACK_LINES 128U' \
    '#define SCROLLBACK_LINE_MAX 1024U' \
    'static char scrollback_lines[SCROLLBACK_LINES][SCROLLBACK_LINE_MAX];' \
    'static void scrollback_capture_byte(unsigned char value)' \
    'if (scrollback_muted || console_replaying) {' \
    "if (value != '\\n' && console_keyboard_echo_active) {" \
    'static void scroll_command(void)' \
    'console_command_is(input_line, "scroll")' \
    'pager_handle_event(event);' \
    'KEYBOARD_EVENT_HOME:' \
    'KEYBOARD_EVENT_END:' \
    'static size_t pager_columns(void)' \
    'scrollback_prefix_bytes'; do
    grep -Fq -- "$item618_ring" kernel/src/console.c || {
        echo "ITEM-618: консоль не содержит:" "$item618_ring" >&2
        exit 1
    }
done
# The viewer must not block: no sleep/spin loop in the pager path.
if grep -n 'pager' kernel/src/console.c | grep -Eq 'while \(1|for \(;;\)|timer_delay|sleep'; then
    echo "ITEM-618: pager содержит блокирующее ожидание" >&2
    exit 1
fi

# 3. The key hint is translated, not a new English sentence.
for item618_catalog in \
    'console.prose.scroll_hint' \
    'console.prose.scroll_empty' \
    'UP/DOWN: SCROLL' \
    'ВВЕРХ/ВНИЗ: ПРОКРУТКА'; do
    grep -Fq -- "$item618_catalog" kernel/src/appliance_language.c || {
        echo "ITEM-618: каталог не содержит:" "$item618_catalog" >&2
        exit 1
    }
done

# 4. The host proof drives the real command and keys; the QEMU proof captures
#    the framebuffer and proves the top/bottom pages are real screens.
for item618_host in \
    'test_scrollback_pager' \
    'PAGE-LINE-000' \
    'PAGE-LINE-119' \
    'the scroll view opens on the newest line' \
    'Q returns to the live prompt'; do
    grep -Fq -- "$item618_host" tests/console-host.c || {
        echo "ITEM-618: console host proof не содержит:" "$item618_host" >&2
        exit 1
    }
done
[[ -f scripts/test-qemu-console-scrollback.sh ]] || {
    echo "ITEM-618: нет QEMU framebuffer proof" >&2
    exit 1
}
for item618_qemu in \
    'screendump' \
    'send("network\n")' \
    'send("scroll\n")' \
    'sendkey home' \
    'sendkey end' \
    'sendkey q'; do
    grep -Fq -- "$item618_qemu" scripts/test-qemu-console-scrollback.sh || {
        echo "ITEM-618: QEMU proof не содержит:" "$item618_qemu" >&2
        exit 1
    }
done
for item618_target in \
    'test-console-host' \
    'test-qemu-console-scrollback'; do
    grep -Fq -- "$item618_target" Makefile || {
        echo "ITEM-618: Makefile не содержит:" "$item618_target" >&2
        exit 1
    }
done
