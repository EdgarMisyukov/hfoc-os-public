#!/usr/bin/env bash
# ITEM-509 increment 19: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 19 (509.7c-1b-5): the install banner (ITEM-417) reads its
# two sentences from the one catalog in the caller's language.  The English
# column is byte for byte the literals the banner carried, so the QEMU proof
# that reads the painted banner and the `[PRODUCTION-INSTALL] notice screen=...`
# record do not move; the font carries Cyrillic (increment 3) and the text path
# decodes it (increment 17), so a Russian banner is drawn, not mangled.  Since
# increment 27 the flow passes the language the medium declared (a medium that
# declares none keeps the default).  A language the catalog cannot answer
# returns NULL from both accessors and the banner refuses - never half a screen
# silently in English.
for item509_inc19_file in \
    kernel/include/installer_notice.h \
    kernel/src/installer_notice.c \
    kernel/src/production_install_flow.c \
    tests/installer-notice-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc19_file ]] || {
        echo "ITEM-509 inc19: отсутствует $item509_inc19_file" >&2
        exit 1
    }
done

# 1. The catalog carries the title, English verbatim and Russian beside it.
grep -Fq '{"installer.notice.title", "HFOC OS INSTALLED", "HFOC OS УСТАНОВЛЕНА"}' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc19: каталог не несёт заголовок баннера" >&2
    exit 1
}
grep -Fq '{"installer.notice.remove_medium",' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc19: каталог потерял подвал баннера" >&2
    exit 1
}

# 2. Both accessors read the catalog, and the literals they replaced are gone
#    from the banner, so key and sentence cannot drift apart.
grep -Fq 'appliance_language_string(language, "installer.notice.title")' \
    kernel/src/installer_notice.c || {
    echo "ITEM-509 inc19: заголовок баннера не читает каталог" >&2
    exit 1
}
grep -Fq 'appliance_language_string(language, "installer.notice.remove_medium")' \
    kernel/src/installer_notice.c || {
    echo "ITEM-509 inc19: подвал баннера не читает каталог" >&2
    exit 1
}
for item509_inc19_gone in \
    '"HFOC OS INSTALLED"' \
    '"REMOVE THE INSTALLATION MEDIUM WHILE THE MACHINE RESTARTS"'; do
    if grep -Fq -- "$item509_inc19_gone" kernel/src/installer_notice.c ||
        grep -Fq -- "$item509_inc19_gone" kernel/src/production_install_flow.c; then
        echo "ITEM-509 inc19: английская проза осталась литералом: $item509_inc19_gone" >&2
        exit 1
    fi
done
grep -Fq 'const char *installer_notice_title(enum appliance_language language)' \
    kernel/include/installer_notice.h || {
    echo "ITEM-509 inc19: заголовок не объявлен" >&2
    exit 1
}
grep -Fq 'const char *installer_notice_footer(enum appliance_language language)' \
    kernel/include/installer_notice.h || {
    echo "ITEM-509 inc19: подвал не объявлен" >&2
    exit 1
}
grep -Fq 'bool installer_notice_banner(enum appliance_language language,' \
    kernel/include/installer_notice.h || {
    echo "ITEM-509 inc19: баннер не принимает язык" >&2
    exit 1
}
grep -A 1 'installer_notice_banner(' kernel/src/production_install_flow.c | \
    grep -Fq 'production_install_flow.declaration.language,' || {
    echo "ITEM-509 inc19: поток установки не передаёт язык баннеру" >&2
    exit 1
}
# The stale contract note must not keep promising an ASCII-only font: callers
# read the header, not the queue.
if grep -Fq 'has upper-case ASCII and digits' kernel/include/installer_notice.h; then
    echo "ITEM-509 inc19: installer_notice.h всё ещё обещает только ASCII" >&2
    exit 1
fi

# 3. The proof pins the English literals, the Russian column and the named
#    refusal, and the host suite runs it.
for item509_inc19_check in \
    'the English title is the literal the banner always printed' \
    'the English footer is the literal the banner always printed' \
    'the Russian title comes from the catalog column' \
    'the Russian footer comes from the catalog column' \
    'an unknown language answers NULL instead of falling back to ' \
    'a banner with no framebuffer refuses instead of claiming a screen'; do
    grep -Fq -- "$item509_inc19_check" tests/installer-notice-host.c || {
        echo "ITEM-509 inc19: проба не утверждает: $item509_inc19_check" >&2
        exit 1
    }
done
grep -Fq 'test-installer-notice-host:' Makefile || {
    echo "ITEM-509 inc19: нет цели пробы баннера" >&2
    exit 1
}
grep -Fq '$(MAKE) test-installer-notice-host' Makefile || {
    echo "ITEM-509 inc19: проба баннера не в host suite" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c' Makefile || {
    echo "ITEM-509 inc19: проба не линкует каталог" >&2
    exit 1
}

# 4. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 19' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc19: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7c-1b-5' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc19: очередь не называет половину инкремента" >&2
    exit 1
}
