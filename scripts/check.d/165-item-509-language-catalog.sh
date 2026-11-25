#!/usr/bin/env bash
# ITEM-509 increment 1: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 1 opens the item with its foundation: one string catalog
# for the whole interface, the inventory of the surfaces that will read it and
# the code-point inventory the console glyph proof (increment 3) consumes.  The
# increment deliberately wires no surface: every existing rendering and every
# English literal stays byte-for-byte as it was, so no pin or QEMU marker moves
# here.  What is pinned below is the catalog itself (two languages, English the
# default, one durable setting key), the fail-closed lookup that never falls
# back to English, the bounded UTF-8 reader and the refusal to publish a
# partial code-point set.
for item509_inc1_file in \
    kernel/include/appliance_language.h \
    kernel/src/appliance_language.c \
    tests/appliance-language-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc1_file ]] || {
        echo "ITEM-509 inc1: отсутствует $item509_inc1_file" >&2
        exit 1
    }
done

# 1. One catalog, exactly two languages, English the default, one durable
#    setting for the console and the panel together, and the two interfaces the
#    later increments consume.
grep -Fq 'APPLIANCE_LANGUAGE_RU = 1' kernel/include/appliance_language.h || {
    echo "ITEM-509 inc1: каталог не объявляет русский язык" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_LANGUAGE_DEFAULT APPLIANCE_LANGUAGE_EN' \
    kernel/include/appliance_language.h || {
    echo "ITEM-509 inc1: язык по умолчанию не английский" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_LANGUAGE_SETTING_KEY "v1:system.language"' \
    kernel/include/appliance_language.h || {
    echo "ITEM-509 inc1: ключ настройки не v1:system.language" >&2
    exit 1
}
grep -Fq 'appliance_language_utf8_next' kernel/include/appliance_language.h || {
    echo "ITEM-509 inc1: разбор UTF-8 не объявлен" >&2
    exit 1
}
grep -Fq 'appliance_language_ru_codepoints' kernel/include/appliance_language.h || {
    echo "ITEM-509 inc1: инвентарь кодпойнтов не объявлен" >&2
    exit 1
}

# 2. The catalog carries the ten panel sections verbatim and the console and
#    help vocabulary the next increments consume; both languages are present,
#    and the Russian column really is Russian (a spot check with the two
#    letters the glyph proof has to cover).
for item509_inc1_key in \
    '{"panel.section.dashboard", "Dashboard", "Обзор"}' \
    '{"panel.section.network", "Network", "Сеть"}' \
    '{"panel.section.services", "Services", "Службы"}' \
    '{"panel.section.storage", "Storage", "Хранилище"}' \
    '{"panel.section.system", "System", "Система"}' \
    '{"panel.section.security", "Security", "Безопасность"}' \
    '{"panel.section.diagnostics", "Diagnostics", "Диагностика"}' \
    '{"panel.section.updates", "Updates", "Обновления"}' \
    '{"panel.section.recovery", "Recovery", "Восстановление"}' \
    '{"panel.section.routing", "Routing", "Маршрутизация"}' \
    '{"panel.section.account", "Account", "Учётная запись"}' \
    '{"panel.section.certificates", "Certificates", "Сертификаты"}' \
    '{"console.language.usage", "USAGE: language en|ru"' \
    '{"console.language.saved", "LANGUAGE SAVED", "ЯЗЫК СОХРАНЁН"}' \
    '{"help.title", "Help", "Справка"}'; do
    grep -Fq "$item509_inc1_key" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc1: каталог потерял $item509_inc1_key" >&2
        exit 1
    }
done

# 3. The lookup is fail-closed: an absent or empty translation returns NULL and
#    never a fallback, and the translation unit is data and pure functions -
#    no allocator, no console and no panel dependency run through it.
grep -Fq 'if (value == NULL || value[0] == '\''\0'\'') {' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc1: отсутствующий перевод не отвергается" >&2
    exit 1
}
if grep -Eq 'malloc|free\(|console_|appliance_management' \
    kernel/src/appliance_language.c; then
    echo "ITEM-509 inc1: каталог тянет за собой вывод или память" >&2
    exit 1
fi

# 4. The proof drives the catalog, the parser, the UTF-8 reader and the
#    inventory, and the Makefile runs it in both host suites.
grep -Fq 'the inventory refuses a NULL buffer' tests/appliance-language-host.c || {
    echo "ITEM-509 inc1: проба не отвергает NULL-буфер инвентаря" >&2
    exit 1
}
grep -Fq 'a capacity one short is refused, not truncated' \
    tests/appliance-language-host.c || {
    echo "ITEM-509 inc1: проба не проверяет неполный инвентарь" >&2
    exit 1
}
grep -Fq 'an unknown key resolves to nothing' tests/appliance-language-host.c || {
    echo "ITEM-509 inc1: проба не проверяет неизвестный ключ" >&2
    exit 1
}
grep -Fq '.PHONY: test-appliance-language-host' Makefile || {
    echo "ITEM-509 inc1: цель пробы не объявлена" >&2
    exit 1
}
grep -Fq '$(MAKE) test-appliance-language-host' Makefile || {
    echo "ITEM-509 inc1: проба не включена в набор" >&2
    exit 1
}

# 5. The inventory document names every surface and the increment that migrates
#    it, and the three documents record this increment.
for item509_inc1_surface in \
    'Console commands and replies' \
    'Console glyphs' \
    'Panel sections and pages' \
    'Help content' \
    'Installer screens'; do
    grep -Fq "$item509_inc1_surface" docs/hfoc-language-surfaces.md || {
        echo "ITEM-509 inc1: инвентарь не называет поверхность $item509_inc1_surface" >&2
        exit 1
    }
done
for item509_inc1_step in 509.2 509.3 509.4 509.5 509.6 509.7; do
    grep -Fq "$item509_inc1_step" docs/hfoc-language-surfaces.md || {
        echo "ITEM-509 inc1: инвентарь не называет шаг $item509_inc1_step" >&2
        exit 1
    }
done
for item509_inc1_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 1' "$item509_inc1_doc" || {
        echo "ITEM-509 inc1: $item509_inc1_doc не называет инкремент" >&2
        exit 1
    }
done
