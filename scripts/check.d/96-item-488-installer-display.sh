#!/usr/bin/env bash
# ITEM-488: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The installer draws on the firmware's framebuffer or on text, and never on a
# GPU-specific driver.
#
# The installer has to run on machines HFOC has no driver for, which is only
# possible if its display comes from the firmware and its UI talks to an
# abstraction instead of a pixel address.  This slice pins the boundary and the
# rules that keep it honest:
#
#   * `display.c` is a value machine - it must not include the framebuffer or
#     the boot protocol's header, so every rule about what may be drawn on is
#     provable on a host;
#   * `display_firmware.c` is the only display file that knows the boot
#     protocol's framebuffer type;
#   * a framebuffer is `AVAILABLE` only after the address, the dimensions, the
#     pixel format and the pitch all passed: detection alone never reports a
#     pass;
#   * a missing or refused framebuffer binds the text path - and the boot no
#     longer panics over one;
#   * the installer UI renders through the abstraction, and every capability it
#     does not implement says `NOT_IMPLEMENTED` rather than passing;
#   * no GPU-specific driver, DRM/KMS, Mesa or desktop stack is added anywhere.

for item488_file in \
    kernel/include/display.h \
    kernel/include/display_firmware.h \
    kernel/include/installer_ui.h \
    kernel/src/display.c \
    kernel/src/display_firmware.c \
    kernel/src/installer_ui.c \
    tests/display-host.c \
    tests/installer-ui-host.c \
    docs/hfoc-installer-display.md; do
    test -f "$item488_file" || {
        echo "ITEM-488: отсутствует файл: $item488_file" >&2
        exit 1
    }
done

# The abstraction is a value machine: no framebuffer, no boot protocol, no GPIO
# of any vendor.  A host proof can therefore drive every case.
for item488_forbidden in framebuffer.h limine_min.h limine.h amdgpu nvidia; do
    if grep -Fq "$item488_forbidden" kernel/src/display.c; then
        echo "ITEM-488: display.c знает про $item488_forbidden и не доказуем на хосте" >&2
        exit 1
    fi
done
for item488_forbidden in framebuffer.h limine_min.h; do
    if grep -Fq "$item488_forbidden" kernel/src/installer_ui.c; then
        echo "ITEM-488: installer_ui.c рисует мимо абстракции ($item488_forbidden)" >&2
        exit 1
    fi
done
# One file knows the firmware's type, and it is the backend.
grep -Fq 'limine_min.h' kernel/src/display_firmware.c || {
    echo "ITEM-488: backend не получает framebuffer из boot protocol" >&2
    exit 1
}

# Fail closed: every check that must pass before AVAILABLE, and the text path
# bound by each refusal.
for item488_check in \
    'framebuffer->address == NULL' \
    'DISPLAY_MAX_DIMENSION' \
    'framebuffer->bpp != DISPLAY_SUPPORTED_BPP' \
    'memory_model != DISPLAY_MEMORY_MODEL_RGB' \
    'display_classify_format' \
    'display_pitch_is_valid' \
    'display_bind_text_fallback("INVALID_PITCH", false)' \
    'display_bind_text_fallback("NO_FRAMEBUFFER", false)'; do
    grep -Fq "$item488_check" kernel/src/display.c || {
        echo "ITEM-488: проверка framebuffer'а пропала: $item488_check" >&2
        exit 1
    }
done
grep -Fq 'display_state.status = DISPLAY_STATUS_AVAILABLE;' \
    kernel/src/display.c || {
    echo "ITEM-488: AVAILABLE не выставляется после проверок" >&2
    exit 1
}
# Detection is not a pass: the record names the status, the reason and where the
# flow goes when there is no framebuffer.
for item488_record in \
    '"[DISPLAY] backend="' \
    '" status="' \
    '" resolution="' \
    '" pitch="' \
    '" bpp="' \
    '" format="' \
    '" fallback="' \
    '" reason="' \
    'DISPLAY_RECORD_BYTES'; do
    grep -Fq -- "$item488_record" kernel/src/display.c || {
        echo "ITEM-488: запись [DISPLAY] не называет: $item488_record" >&2
        exit 1
    }
done

# No framebuffer is a reading, not a panic: the two panics this item replaced
# must be gone, and the text path bound in their place.
if grep -Fq 'kernel_panic("No framebuffer received")' kernel/src/kernel.c; then
    echo "ITEM-488: отсутствие GOP всё ещё паникует" >&2
    exit 1
fi
if grep -Fq 'kernel_panic("Unsupported framebuffer; expected 32 bpp")' \
    kernel/src/kernel.c; then
    echo "ITEM-488: непригодный framebuffer всё ещё паникует" >&2
    exit 1
fi
grep -Fq 'display_probe_firmware(NULL, "NO_FRAMEBUFFER", true)' \
    kernel/src/kernel.c || {
    echo "ITEM-488: boot не связывает text fallback" >&2
    exit 1
}
grep -Fq 'framebuffer_release();' kernel/src/kernel.c || {
    echo "ITEM-488: отказанный framebuffer остаётся связанным" >&2
    exit 1
}
grep -Fq 'display_publish();' kernel/src/kernel.c || {
    echo "ITEM-488: чтение [DISPLAY] не публикуется" >&2
    exit 1
}
# A boot fact published once is overwritten by the run's own diagnostics: the
# reading is re-published on a slower cadence so the newest copy stays in the
# journal's bounded ring (hw17 booted on 3840x2160 and the record was gone by
# the time the machine was switched off).
grep -Fq 'KERNEL_DISPLAY_PUBLISH_EVERY' kernel/src/kernel.c || {
    echo "ITEM-488: чтение [DISPLAY] не повторяется и теряется в кольце" >&2
    exit 1
}
grep -Fq 'log_production_loop_display();' kernel/src/kernel.c || {
    echo "ITEM-488: периодическая публикация [DISPLAY] не вызывается" >&2
    exit 1
}
# ITEM-509 increment 27: the screen takes the language the caller names; the
# caller is now the declaration the medium carried, read before the first
# screen, and a medium that declares none keeps the default language.
grep -Fq 'display_show_installer_foundation(kernel_installer_declared_language());' \
    kernel/src/kernel.c || {
    echo "ITEM-488: экран установщика не рисуется через абстракцию" >&2
    exit 1
}

# The UI: one renderer, the capabilities this item does not own named as such,
# and the same flow in text.
for item488_ui in \
    'INSTALLER_UI_STATE_NOT_IMPLEMENTED' \
    '"NOT_IMPLEMENTED"' \
    'installer_ui_lines' \
    'installer_ui_foundation' \
    'installer_ui_record'; do
    grep -Fq "$item488_ui" kernel/src/installer_ui.c || {
        echo "ITEM-488: UI не публикует: $item488_ui" >&2
        exit 1
    }
done
grep -Fq 'installer_ui_lines(&model, language, lines)' \
    kernel/src/display_firmware.c || {
    echo "ITEM-488: текстовый путь не рисует тот же экран" >&2
    exit 1
}

# No GPU-specific driver, DRM/KMS, Mesa or desktop graphics anywhere.
for item488_source in kernel/src/*; do
    case "${item488_source##*/}" in
    *amdgpu* | *nvidia* | *i915* | *drm* | *mesa* | *wayland* | *x11*)
        echo "ITEM-488: в ядре появился GPU/desktop стек: $item488_source" >&2
        exit 1
        ;;
    esac
done

# The evidence: the two host proofs, registered with the suite.
for item488_case in \
    'test_gop_framebuffer_is_available' \
    'test_absent_framebuffer_falls_back' \
    'test_unsupported_formats_degrade' \
    'test_invalid_pitch_degrades' \
    'test_a_failed_probe_clears_the_previous_one'; do
    grep -Fq "$item488_case" tests/display-host.c || {
        echo "ITEM-488: host-проба display не доказывает: $item488_case" >&2
        exit 1
    }
done
grep -Fq 'test_text_fallback_screen' tests/installer-ui-host.c || {
    echo "ITEM-488: host-проба UI не доказывает текстовый экран" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile text is literal, not expanded
grep -Fq '$(MAKE) test-display-host' Makefile || {
    echo "ITEM-488: host-проба display не в suite" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile text is literal, not expanded
grep -Fq '$(MAKE) test-installer-ui-host' Makefile || {
    echo "ITEM-488: host-проба UI не в suite" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item488_doc_word in ITEM-488 GOP 'display abstraction' NOT_IMPLEMENTED \
    'QEMU/OVMF proof != physical hardware acceptance' 'NOT TESTED'; do
    grep -Fq -- "$item488_doc_word" docs/hfoc-installer-display.md || {
        echo "docs/hfoc-installer-display.md не фиксирует ITEM-488:" \
            "$item488_doc_word" >&2
        exit 1
    }
done
