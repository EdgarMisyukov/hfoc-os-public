#!/usr/bin/env bash
# ITEM-509 increment 15: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 15 (509.7c-1b-2): two more installer surfaces read the one
# catalog - the refusal page (`installer_storage_refusal_show`) and the line the
# disk screen shows when the medium's declared disk is not on this machine
# (`production_install_media_render_choice_note`).  Both take the language as an
# explicit parameter; since increment 27 the boot and the install flow pass the
# language the medium declared (a medium that declares none keeps the default).
# The English page and line are byte for byte what they were, so the QEMU pins
# in `scripts/test-qemu-installer-disk-screen.sh` stay untouched.
for item509_inc15_file in \
    kernel/src/installer_storage_screen.c \
    kernel/src/production_install_media.c \
    kernel/include/installer_storage_screen.h \
    kernel/include/production_install_media.h \
    tests/production-install-media-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc15_file ]] || {
        echo "ITEM-509 inc15: отсутствует $item509_inc15_file" >&2
        exit 1
    }
done

# 1. The refusal page reads the catalog in the caller's language and refuses a
#    missing translation by name; the refusal reason itself stays the machine
#    statement the owner published.  ITEM-541 moved the page's lines into the
#    host-proven builder in `installer_storage.c`, so that is where the catalog
#    keys and the named refusal live now.
for item509_inc15_refusal_key in \
    '"installer.refusal.title"' \
    '"installer.refusal.reason_prefix"' \
    '"installer.refusal.medium"' \
    '"installer.refusal.press_key"'; do
    grep -Fq -- "$item509_inc15_refusal_key" \
        kernel/src/installer_storage.c || {
        echo "ITEM-509 inc15: страница отказа не читает каталог: $item509_inc15_refusal_key" >&2
        exit 1
    }
done
grep -Fq 'INSTALLER TRANSLATION MISSING' \
    kernel/src/installer_storage.c || {
    echo "ITEM-509 inc15: страница отказа не называет отказ перевода" >&2
    exit 1
}
# ITEM-518: the signature now also takes the observed storage report the page
# prints; the language parameter and the call are otherwise the same contract.
grep -Fq 'void installer_storage_refusal_show(' \
    kernel/include/installer_storage_screen.h || {
    echo "ITEM-509 inc15: страница отказа не объявлена" >&2
    exit 1
}
grep -Fq 'const struct appliance_storage_report *observed,' \
    kernel/include/installer_storage_screen.h || {
    echo "ITEM-518: страница отказа не принимает наблюдённое хранилище" >&2
    exit 1
}
grep -Fq 'enum appliance_language language);' \
    kernel/include/installer_storage_screen.h || {
    echo "ITEM-509 inc15: страница отказа не принимает язык" >&2
    exit 1
}
grep -Fq 'installer_storage_refusal_show(refusal, &observed,' \
    kernel/src/kernel.c || {
    echo "ITEM-509 inc15: загрузка не передаёт язык странице отказа" >&2
    exit 1
}
grep -Fq 'kernel_installer_declared_language());' kernel/src/kernel.c || {
    echo "ITEM-509 inc15: страница отказа не читает язык объявления" >&2
    exit 1
}
if grep -Fq '"Installation refused"' kernel/src/installer_storage_screen.c; then
    echo "ITEM-509 inc15: английская проза отказа осталась литералом" >&2
    exit 1
fi

# 2. The declared-disk note reads the catalog and refuses a missing translation
#    with the named status; the declared target and `NONE` stay machine values.
for item509_inc15_note_key in \
    '"installer.media.declared_disk_prefix"' \
    '"installer.media.not_on_machine_suffix"'; do
    grep -Fq -- "$item509_inc15_note_key" \
        kernel/src/production_install_media.c || {
        echo "ITEM-509 inc15: строка выбора не читает каталог: $item509_inc15_note_key" >&2
        exit 1
    }
done
grep -Fq 'return PRODUCTION_INSTALL_MEDIA_BAD_TEXT;' \
    kernel/src/production_install_media.c || {
    echo "ITEM-509 inc15: строка выбора не отказывает по имени" >&2
    exit 1
}
grep -Fq 'production_install_flow.declaration.language,' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-509 inc15: поток не передаёт язык строке выбора" >&2
    exit 1
}
if grep -Fq '"Declared disk "' kernel/src/production_install_media.c; then
    echo "ITEM-509 inc15: английская строка выбора осталась литералом" >&2
    exit 1
fi

# 3. The proof renders the Russian line and the named refusal, and the target
#    links the catalog.
grep -Fq 'APPLIANCE_LANGUAGE_RU,' tests/production-install-media-host.c || {
    echo "ITEM-509 inc15: проба не рисует русскую строку" >&2
    exit 1
}
grep -Fq 'Объявленный диск DEADBEEF00000001 отсутствует на этой ' \
    tests/production-install-media-host.c || {
    echo "ITEM-509 inc15: проба не называет русскую строку" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc15: проба не линкует каталог" >&2
    exit 1
}

# 4. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 15' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc15: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7c-1b-2' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc15: очередь не называет половину инкремента" >&2
    exit 1
}
