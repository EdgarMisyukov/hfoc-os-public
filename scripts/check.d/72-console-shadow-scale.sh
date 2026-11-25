#!/usr/bin/env bash
# ITEM-466: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# ITEM-466 (partial): the console scrolls through a cacheable RAM shadow, and
# the glyph scale comes from the mode.
#
# The 2026-09-21 hardware journal (`4df5c4b9`) measured
# `[CONSOLE] scroll rows=17 bytes=48046080 us=2171 max_us=16315`: one scrolled
# line read the console body back out of uncached video memory, and the main
# loop - which also polls the keyboard - waited for it ("the keyboard
# practically does not type").  The same journal shows the forced 1024x768
# image stretched on a 4K monitor ("the characters became huge").
#
# The claims pinned here are what this branch already does:
#
#   * framebuffer.c keeps a caller-owned shadow of the whole surface and every
#     console write lands in both copies;
#   * framebuffer_scroll_up() moves the rows inside the shadow and then writes
#     video memory, so a scroll never reads it back; pixel_read() - the
#     software pointer's save/restore - reads the shadow too;
#   * kernel.c allocates the shadow from the physical allocator after pmm_init
#     and publishes `[FB] shadow bytes= pages= physical= bound=0|1`; a refused
#     allocation is not fatal and leaves the old read-back path;
#   * framebuffer_font_scale() is the pure rule - the largest scale in [1,3]
#     whose 8x16 grid keeps at least 80 columns and 24 rows - and console.c
#     takes its scale and both cell dimensions from the actual mode;
#   * the `[FB]` record carries ` scale=N cell_px=WxH` after `fallback=`;
#   * the host proof poisons the video rows the old code would have read and
#     requires the painted colours from the shadow, and pins the four modes the
#     owner named.

for console_466_file in \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    kernel/src/kernel.c \
    kernel/src/console.c \
    tests/framebuffer-scroll-host.c \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $console_466_file ]] || {
        echo "ITEM-466: отсутствует $console_466_file" >&2
        exit 1
    }
done

# The pure scale rule, its documented floor and cap, and the bound surface.
grep -Fq '#define FRAMEBUFFER_FONT_CELL_WIDTH 8U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: не зафиксирована ширина клетки шрифта" >&2
    exit 1
}
grep -Fq '#define FRAMEBUFFER_FONT_CELL_HEIGHT 16U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: не зафиксирована высота клетки шрифта" >&2
    exit 1
}
grep -Fq '#define FRAMEBUFFER_FONT_MIN_COLUMNS 80U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: не зафиксирован минимум колонок" >&2
    exit 1
}
grep -Fq '#define FRAMEBUFFER_FONT_MIN_ROWS 30U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: не зафиксирован минимум строк" >&2
    exit 1
}
grep -Fq '#define FRAMEBUFFER_FONT_SCALE_MAX 3U' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: не зафиксирован предел шкалы" >&2
    exit 1
}
grep -Fq 'uint32_t framebuffer_font_scale(uint64_t width, uint64_t height);' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: framebuffer.h не объявляет правило шкалы" >&2
    exit 1
}
grep -Fq 'uint32_t framebuffer_font_scale(uint64_t width, uint64_t height) {' \
    kernel/src/framebuffer.c || {
    echo "ITEM-466: правило шкалы не определено" >&2
    exit 1
}

# The shadow: bound surface, both-copy writes, and the write-only scroll.
grep -Fq 'uint64_t framebuffer_shadow_required_bytes(void);' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: нет запроса размера тени" >&2
    exit 1
}
grep -Fq 'bool framebuffer_shadow_init(void *memory, uint64_t bytes);' \
    kernel/include/framebuffer.h || {
    echo "ITEM-466: тень не привязывается вызывающим" >&2
    exit 1
}
grep -Fq 'if (shadow_ready) {' kernel/src/framebuffer.c || {
    echo "ITEM-466: у прокрутки нет теневого пути" >&2
    exit 1
}
grep -Fq 'framebuffer_copy_row(framebuffer_base() + y * pitch,' \
    kernel/src/framebuffer.c || {
    echo "ITEM-466: прокрутка не пишет видео из тени" >&2
    exit 1
}
grep -Fq 'return ((const uint32_t *)shadow_memory)[' \
    kernel/src/framebuffer.c || {
    echo "ITEM-466: чтение указателя не идёт через тень" >&2
    exit 1
}

# The boot binds the shadow from the physical allocator and names the outcome.
grep -Fq 'pmm_alloc_contiguous_pages((uint32_t)shadow_pages);' \
    kernel/src/kernel.c || {
    echo "ITEM-466: тень не выделяется из физического аллокатора" >&2
    exit 1
}
grep -Fq 'serial_write("[FB] shadow bytes=");' kernel/src/kernel.c || {
    echo "ITEM-466: boot не публикует [FB] shadow" >&2
    exit 1
}
grep -Fq 'framebuffer_shadow_init(shadow_virtual, shadow_needed)' \
    kernel/src/kernel.c || {
    echo "ITEM-466: boot не привязывает тень" >&2
    exit 1
}

# The console takes its geometry from the mode and publishes it.
grep -Fq 'console_scale = framebuffer_font_scale(width, height);' \
    kernel/src/console.c || {
    echo "ITEM-466: шкала не берётся из режима в записи [FB]" >&2
    exit 1
}
grep -Fq 'console_scale = framebuffer_font_scale(framebuffer_width(),' \
    kernel/src/console.c || {
    echo "ITEM-466: шкала не берётся из фактического режима" >&2
    exit 1
}
grep -Fq '" scale=");' kernel/src/console.c || {
    echo "ITEM-466: [FB] не публикует scale=" >&2
    exit 1
}
grep -Fq '" cell_px=");' kernel/src/console.c || {
    echo "ITEM-466: [FB] не публикует cell_px=" >&2
    exit 1
}
if grep -Fq '#define CELL_HEIGHT (9U * SCALE)' kernel/src/console.c; then
    echo "ITEM-466: шкала консоли всё ещё компиляционная константа" >&2
    exit 1
fi

# The host proof drives the real framebuffer.c: it poisons the video source
# rows and requires the colours the shadow carried, and it pins the modes.
for console_466_proof in \
    'the scrolled row came from the shadow, not from the poisoned ' \
    'the shadow carries the same picture the screen does' \
    'a buffer smaller than the surface is refused' \
    '1024x768 gets a 1x cell' \
    '1366x768 gets a 1x cell' \
    '1920x1080 gets a 2x cell' \
    '3840x2160 gets a 3x cell' \
    'the chosen scale is the largest one that keeps the '; do
    grep -Fq -- "$console_466_proof" tests/framebuffer-scroll-host.c || {
        echo "ITEM-466: host-проба не требует:" "$console_466_proof" >&2
        exit 1
    }
done

grep -Fq 'ITEM-466' docs/hfoc-appliance-boot-console.md || {
    echo "ITEM-466: документация консоли не описывает тень и шкалу" >&2
    exit 1
}
