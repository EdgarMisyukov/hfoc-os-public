#!/usr/bin/env bash
# ITEM-463/464/466: contracted slice of the HFOC OS check suite. Sourced in
#              pinned order by scripts/check.sh; lint it on its own too.

# The console chooses its own mode and scale, keeps up, and its costs are
# microseconds.
#
# The owner's 2026-09-20 durable journal (ITEM-460) carried the fact ITEM-463
# rests on:
#
#   [FB] width=3840 height=2160 pitch=15360 cells=320x120 bpp=32
#   [CONSOLE] paint lines=1 chars=25 ms=0 max_ms=0
#
# The console ran at the firmware's maximum mode, so a scroll moved ~33 MB and
# the pointer saved a 4K block; and the costs were milliseconds on a 100 Hz
# tick, so every paint read as zero and the budget bounded nothing.  ITEM-463
# then found that a *global* `resolution:` line is not the entry's option, so
# the installed system still booted 4K.  ITEM-464 put the request on the entry
# in both configurations, but that fixed one mode the monitor stretched.
# ITEM-466 (round 5) removes the hardcode and lets the firmware's own preference
# boot.  The claims pinned here:
#
#   * neither `boot/limine.conf` nor `boot/hfoc-installed.conf` asks for a
#     resolution, and `installer_boot_menu.c` renders no mode line; the boot
#     publishes the preference, the available list, the choice, the actual mode,
#     the fallback flag and the automatic scale/cell/grid:
#     `[FB] ... modes=N chosen=WxH requested=WxH actual=WxH fallback=0|1
#      scale=N cell_px=WxH preferred=WxH available_count=N available=WxH,...`
#     (`requested=` is the legacy name of `preferred=`);
#   * the mode rule walks the Limine mode list, names the first (most preferred)
#     32-bpp mode as the preference, and falls back to the largest available one
#     with `fallback=1` when the preference was refused; and the console drains
#     the write-combining framebuffer (`sfence` plus a pixel read) before a
#     duration is read;
#   * the costs are microseconds from the processor's cycle counter (`rdtsc`),
#     whose frequency is measured against the PIT tick after interrupts are
#     enabled - not the 100 Hz tick and not an assumed constant;
#   * `[CONSOLE] paint lines= chars= us= max_us=`,
#     `[CONSOLE] scroll rows= bytes= us= max_us=` and the pointer's
#     `moves=`/`max_us=` name what the screen cost;
#   * the screen has its own output budget: at most 12 records are painted per
#     one-second window, and the suppressed count is published as
#     `[CONSOLE] screen suppressed=` and ` screen_suppressed=` on the heartbeat;
#   * the heartbeat republishes the costs every window, even when the state did
#     not change: `iter_us=`, `iter_max_us=`, `paint_us=`, `paint_max_us=`,
#     `scroll_us=`, `scroll_max_us=`, `ptr_moves=`, `ptr_max_us=`, beside the
#     keyboard's `kbd_dropped=`/`kbd_gap_ms_max=`;
#   * a QEMU proof runs the same media at three EDID preferences (1024x768,
#     1920x1080, the owner's 3840x2160), prints load and types a character
#     stream, and requires the native actual, `fallback=0` and the automatic
#     scale for the first two; the 4K run boots with the 4K EDID but QEMU's VGA
#     lacks the video memory for a 33 MB surface, so it requires the fallback
#     flag and the automatic scale to follow whatever actual mode came back
#     (the pure 3840x2160 -> 3 rule is pinned by the host proof); all three
#     require the paint and loop costs inside their microsecond budgets; the
#     installed system's own QEMU proof (and the production install flow) must
#     boot the native 1920x1080 at scale 2 and stay inside the loop budget;
#   * the fail-closed `Kernel panic` signature carries its trailing space, so a
#     healthy guest's `Kernel panic: none` all-clear no longer swallows the
#     retryable marker-timeout class of the very proof this item runs.
for console_4k_file in \
    boot/limine.conf \
    boot/hfoc-installed.conf \
    kernel/include/arch/x86_64/timer.h \
    kernel/src/arch/x86_64/timer.c \
    kernel/include/console.h \
    kernel/src/console.c \
    kernel/src/kernel.c \
    kernel/include/framebuffer.h \
    kernel/src/framebuffer.c \
    kernel/src/installer_boot_menu.c \
    tests/console-host.c \
    scripts/test-qemu-console-history.sh \
    scripts/test-qemu-installed-boot.sh \
    scripts/test-qemu-production-install-flow.sh \
    scripts/hfoc-qemu-failure-class.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $console_4k_file ]] || {
        echo "ITEM-463/464: отсутствует $console_4k_file" >&2
        exit 1
    }
done

# ITEM-466 (round 5): the console no longer asks for a fixed mode at all.  The
# firmware's own preference boots, and the kernel names it; a `resolution:` line
# would be the hardcode the owner removed.
for console_4k_config in boot/limine.conf boot/hfoc-installed.conf; do
    if grep -Eq '^[[:space:]]*resolution:' "$console_4k_config"; then
        echo "ITEM-466: $console_4k_config снова просит фиксированный режим" >&2
        exit 1
    fi
done

# The cycle clock: the counter, the conversion and the measured frequency.
for console_4k_clock in \
    '__asm__ volatile("rdtsc" : "=a"(low), "=d"(high));' \
    'void timer_cycle_clock_start(void) {' \
    'void timer_cycle_clock_poll(void) {' \
    'platform_reset_delay_ms(TIMER_CYCLE_CALIBRATION_MS);' \
    'uint64_t timer_cycles(void) {' \
    'uint64_t timer_cycles_to_us(uint64_t cycles) {'; do
    grep -Fq -- "$console_4k_clock" kernel/src/arch/x86_64/timer.c || {
        echo "ITEM-463: цикловые часы не содержат:" "$console_4k_clock" >&2
        exit 1
    }
done
for console_4k_clock_decl in \
    'void timer_cycle_clock_start(void);' \
    'void timer_cycle_clock_poll(void);' \
    'uint64_t timer_cycles(void);' \
    'uint64_t timer_cycles_to_us(uint64_t cycles);'; do
    grep -Fq -- "$console_4k_clock_decl" kernel/include/arch/x86_64/timer.h || {
        echo "ITEM-463: timer.h не объявляет:" "$console_4k_clock_decl" >&2
        exit 1
    }
done
# The clock is started after interrupts are enabled and polled in the loop.
grep -Fq 'timer_cycle_clock_start();' kernel/src/kernel.c || {
    echo "ITEM-463: цикловые часы не запущены" >&2
    exit 1
}
grep -Fq 'timer_cycle_clock_poll();' kernel/src/kernel.c || {
    echo "ITEM-463: цикловые часы не калибруются в цикле" >&2
    exit 1
}

# The records are microseconds, and the mode record names the firmware list,
# the selection, the request and the fallback.
for console_4k_record in \
    '"[CONSOLE] paint lines="' \
    '" us="' \
    '" max_us="' \
    '"[CONSOLE] scroll rows="' \
    '"[POINTER] moves="' \
    '" modes="' \
    '" chosen="' \
    '" requested="' \
    '" actual="' \
    '" fallback="' \
    '" scale="' \
    '" cell_px="' \
    '" preferred="' \
    '" available_count="' \
    '" available="' \
    '"[CONSOLE] screen suppressed="'; do
    grep -Fq -- "$console_4k_record" kernel/src/console.c || {
        echo "ITEM-463/464: запись консоли не содержит:" "$console_4k_record" >&2
        exit 1
    }
done
# The scrolls inside a paint burst are charged to the scroll record, not the
# paint record.
grep -Fq 'console_scroll_cycles_total' kernel/src/console.c || {
    echo "ITEM-463: прокрутка не вычитается из стоимости отрисовки" >&2
    exit 1
}
grep -Fq 'console_render_scroll_us(void)' kernel/src/console.c || {
    echo "ITEM-463: нет геттера средней стоимости прокрутки" >&2
    exit 1
}
grep -Fq 'console_render_pointer_max_us(void)' kernel/src/console.c || {
    echo "ITEM-463: нет геттера стоимости указателя" >&2
    exit 1
}
for console_4k_getter in \
    'uint64_t console_render_paint_us(void);' \
    'uint64_t console_render_scroll_us(void);' \
    'uint64_t console_render_scroll_max_us(void);' \
    'uint64_t console_render_pointer_moves(void);' \
    'uint64_t console_render_pointer_max_us(void);'; do
    grep -Fq -- "$console_4k_getter" kernel/include/console.h || {
        echo "ITEM-463: console.h не объявляет:" "$console_4k_getter" >&2
        exit 1
    }
done

# ITEM-464: the selection rule, the mode count and the write-combining drain.
for console_4k_mode in \
    '#define FRAMEBUFFER_REQUESTED_WIDTH 1024U' \
    '#define FRAMEBUFFER_REQUESTED_HEIGHT 768U' \
    'bool framebuffer_mode_is_bounded(uint64_t width, uint64_t height);' \
    'uint64_t framebuffer_mode_count(void);' \
    'bool framebuffer_best_mode(uint64_t *width, uint64_t *height);' \
    'void framebuffer_sync(void);' \
    'static inline bool framebuffer_choose_mode(' \
    'static inline bool framebuffer_choose_preferred(' \
    'bool framebuffer_describe_modes(uint64_t actual_width,' \
    'uint64_t framebuffer_available_text(char *output, size_t capacity);' \
    'mode->width >= FRAMEBUFFER_REQUESTED_WIDTH &&'; do
    grep -Fq -- "$console_4k_mode" kernel/include/framebuffer.h || {
        echo "ITEM-464: framebuffer.h не содержит:" "$console_4k_mode" >&2
        exit 1
    }
done
for console_4k_mode in \
    'uint64_t framebuffer_mode_count(void) {' \
    'bool framebuffer_best_mode(uint64_t *width, uint64_t *height) {' \
    'return framebuffer_choose_mode(active_framebuffer->modes,' \
    'bool framebuffer_describe_modes(uint64_t actual_width,' \
    'return framebuffer_choose_preferred(active_framebuffer->modes,' \
    'uint64_t framebuffer_available_text(char *output, size_t capacity) {' \
    'void framebuffer_sync(void) {' \
    '__asm__ volatile("sfence" : : : "memory");'; do
    grep -Fq -- "$console_4k_mode" kernel/src/framebuffer.c || {
        echo "ITEM-464: framebuffer.c не содержит:" "$console_4k_mode" >&2
        exit 1
    }
done
# The generated loader menu (`\EFI\BOOT\limine.conf`) is what the installed
# firmware actually reads; ITEM-466 removed its fixed `resolution:` line, so the
# entry the installer writes must not carry one.
if grep -Fq '"\n    resolution: "' kernel/src/installer_boot_menu.c; then
    echo "ITEM-466: installer_boot_menu.c снова просит фиксированный режим" >&2
    exit 1
fi
grep -Fq 'INSTALLER_BOOT_MENU_TEXT_APPEND("\n    path: ")' \
    kernel/src/installer_boot_menu.c || {
    echo "ITEM-466: installer_boot_menu.c не пишет пункт меню" >&2
    exit 1
}
# The boot names the firmware's preference and the list it offered; the console
# drains before it reads a cost.
for console_4k_mode in \
    'framebuffer_mode_count()' \
    'framebuffer_describe_modes(framebuffer_width(),'; do
    grep -Fq -- "$console_4k_mode" kernel/src/kernel.c || {
        echo "ITEM-466: boot не публикует выбор режима:" "$console_4k_mode" >&2
        exit 1
    }
done
for console_4k_sync in \
    'framebuffer_sync();' \
    'void framebuffer_sync(void);' ; do
    grep -Fq -- "$console_4k_sync" kernel/src/console.c \
        kernel/include/framebuffer.h || {
        echo "ITEM-464: дренаж framebuffer не подключён:" "$console_4k_sync" >&2
        exit 1
    }
done

# The heartbeat carries the loop, paint, scroll and pointer costs, regularly.
for console_4k_heartbeat in \
    'NET_RECORD_WRITE(" iter_us=");' \
    'NET_RECORD_WRITE(" iter_max_us=");' \
    'NET_RECORD_WRITE(" paint_us=");' \
    'NET_RECORD_WRITE(" paint_max_us=");' \
    'NET_RECORD_WRITE(" scroll_us=");' \
    'NET_RECORD_WRITE(" scroll_max_us=");' \
    'NET_RECORD_WRITE(" ptr_moves=");' \
    'NET_RECORD_WRITE(" ptr_max_us=");' \
    'NET_RECORD_WRITE(" screen_suppressed=");' \
    'NET_RECORD_WRITE(" kbd_dropped=");' \
    'NET_RECORD_WRITE(" kbd_gap_ms_max=");'; do
    grep -Fq -- "$console_4k_heartbeat" kernel/src/kernel.c || {
        echo "ITEM-463: heartbeat не публикует:" "$console_4k_heartbeat" >&2
        exit 1
    }
done

# The QEMU proof runs three EDIDs, reads microseconds, and checks the
# preference, the list, the selection, the scale and the fallback flag.
for console_4k_qemu in \
    'SCROLL_US_BUDGET = 250000' \
    'SCROLL_US_GROWTH_FACTOR = 5' \
    'PAINT_US_BUDGET = 50000' \
    'ITER_US_BUDGET = 500000' \
    'HFOC_CONSOLE_HISTORY_EXPECT_FALLBACK' \
    'HFOC_CONSOLE_HISTORY_EXPECT_SCALE' \
    'preferred=(\d+)x(\d+)' \
    'available_count=(\d+)' \
    'requested=(\d+)x(\d+)' \
    'chosen=(\d+)x(\d+)' \
    'normalised_serial_bytes' \
    'console_split_fixture' \
    'is not the' \
    'the display fallback flag' \
    'contradicts the preferred' \
    'the cell' \
    'the console queue dropped'; do
    grep -Fq -- "$console_4k_qemu" scripts/test-qemu-console-history.sh || {
        echo "ITEM-466: QEMU-проба не требует:" "$console_4k_qemu" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the Makefile text is what the search must match
grep -Fq "HFOC_CONSOLE_HISTORY_DISPLAY='VGA,edid=on,xres=3840,yres=2160'" \
    Makefile || {
    echo "ITEM-463: Makefile не прогоняет 4K" >&2
    exit 1
}
grep -Fq "HFOC_CONSOLE_HISTORY_EXPECT_DISPLAY='1920x1080'" Makefile || {
    echo "ITEM-466: Makefile не требует нативный 1920x1080" >&2
    exit 1
}
grep -Fq 'expected_scale_for' scripts/test-qemu-console-history.sh || {
    echo "ITEM-466: QEMU-проба не проверяет шкалу по фактическому режиму" >&2
    exit 1
}
grep -Fq "HFOC_CONSOLE_HISTORY_EXPECT_SCALE='1'" Makefile || {
    echo "ITEM-466: Makefile не требует шкалу 1 на 1024x768" >&2
    exit 1
}
grep -Fq "HFOC_CONSOLE_HISTORY_EXPECT_SCALE='2'" Makefile || {
    echo "ITEM-466: Makefile не требует шкалу 2 на 1920x1080" >&2
    exit 1
}
# The host proof states the microsecond record, the mode list and the selection.
for console_4k_host in \
    'us=1 max_us=1' \
    'fallback=1 scale=2 cell_px=16x32 preferred=1024x768' \
    'test_framebuffer_mode_choice' \
    'the smallest mode at least as large as the floor is chosen' \
    'test_framebuffer_preferred_mode_choice' \
    'the native mode is the choice and not a fallback' \
    'a refused preference falls back to the largest available mode' \
    'test_console_screen_budget' \
    'the records over the screen budget are counted as suppressed' \
    'a suppressed record still reaches the durable journal' \
    'the pointer publishes its own move count and cost'; do
    grep -Fq -- "$console_4k_host" tests/console-host.c || {
        echo "ITEM-466: host-проба не требует:" "$console_4k_host" >&2
        exit 1
    }
done

# ITEM-466: the installed system's own QEMU proof requires the native mode it
# got, the mode list, the selection and the loop budget.
for console_4k_installed in \
    'VGA,edid=on,xres=1920,yres=1080' \
    'fallback=\d+ scale=\d+ cell_px=\d+x\d+ preferred=\d+x\d+' \
    'ITER_US_BUDGET = 500000' \
    'the installed system booted' \
    'the native mode was not honoured' \
    'the installed firmware mode list was empty'; do
    grep -Fq -- "$console_4k_installed" \
        scripts/test-qemu-installed-boot.sh || {
        echo "ITEM-466: installed-boot проба не требует:" \
            "$console_4k_installed" >&2
        exit 1
    }
done
# And the production install flow reads the real installed template back out of
# the ESP and boots it under the same display, with no hardcoded mode.
for console_4k_installed_flow in \
    'VGA,edid=on,xres=1920,yres=1080' \
    'resolution:' \
    'the installed system booted' \
    'the native mode was not honoured' \
    'the installed main loop took'; do
    grep -Fq -- "$console_4k_installed_flow" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "ITEM-466: install-flow проба не требует:" \
            "$console_4k_installed_flow" >&2
        exit 1
    }
done

# The microsecond clock made every noisy scroll a new maximum, so the "worse
# than reported" trigger needs a margin; and the storage-boot proof strips the
# `[CONSOLE] scroll` record that can split its keyboard echo.
grep -Fq 'console_scroll_reported_max_us / 2U' kernel/src/console.c || {
    echo "ITEM-463: нет порога на 'хуже опубликованного' для прокрутки" >&2
    exit 1
}
for console_4k_split_reader in \
    'scripts/test-qemu-production-storage-boot.sh' \
    'scripts/test-qemu-console-history.sh'; do
    grep -Fq '\[CONSOLE\] scroll' "$console_4k_split_reader" || {
        echo "ITEM-463: $console_4k_split_reader не снимает запись прокрутки" >&2
        exit 1
    }
done

# The classifier's panic signature no longer swallows the all-clear.
grep -Fq "'Kernel panic '" scripts/hfoc-qemu-failure-class.sh || {
    echo "ITEM-463: признак паники снова ловит all-clear" >&2
    exit 1
}
grep -Fq 'Kernel panic: none' scripts/check.d/45-qemu-marker-timeout-retry.sh || {
    echo "ITEM-463: срез 45 не пинит all-clear" >&2
    exit 1
}

# The item is recorded where a maintainer looks.
for console_4k_doc in \
    'ITEM-464' \
    'modes=' \
    'chosen=' \
    'requested=' \
    'max_us=' \
    'fallback'; do
    grep -Fq -- "$console_4k_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-464:" \
            "$console_4k_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-464' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-464" >&2
    exit 1
}
grep -Fq 'ITEM-464' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-464" >&2
    exit 1
}
