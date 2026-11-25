#!/usr/bin/env bash
# ITEM-509 increment 2: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 2 makes the interface language durable without moving a
# single rendered byte: the language is the optional second line of the system
# record, so a record that never chose a language keeps the exact hostname-only
# bytes of the previous format and an old-format record still reads with the
# default `en`.  No surface reads the setting yet - the console command follows
# the glyph increment, because Russian text cannot be drawn before the glyphs
# exist - and that boundary is pinned here too.
for item509_inc2_file in \
    kernel/include/appliance_config_validation.h \
    kernel/src/appliance_config_validation.c \
    kernel/include/appliance_persistent_config.h \
    kernel/src/appliance_persistent_config.c \
    kernel/include/appliance_config_integrity.h \
    kernel/src/appliance_config_integrity.c \
    tests/appliance-config-validation-host.c \
    tests/appliance-persistent-config-host.c \
    tests/appliance-config-integrity-host.c \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc2_file ]] || {
        echo "ITEM-509 inc2: отсутствует $item509_inc2_file" >&2
        exit 1
    }
done

# 1. The record grammar: the language is the second `v1:system.` key, its value
#    is the catalog's two codes, and a third line is refused rather than
#    ignored.
grep -Fq '#define APPLIANCE_CONFIG_VALIDATION_LANGUAGE_PREFIX' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-509 inc2: ключ языка не объявлен" >&2
    exit 1
}
grep -Fq '"v1:system.language="' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-509 inc2: ключ языка не v1:system.language=" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE = 8' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-509 inc2: отказ по языку не назван" >&2
    exit 1
}
grep -Fq 'appliance_language_parse(' kernel/src/appliance_config_validation.c || {
    echo "ITEM-509 inc2: значение языка проверяется не каталогом" >&2
    exit 1
}
# ITEM-516 added a third optional key, so the parser now walks the optional
# lines and refuses a repeated key; the subject of this pin - a third line is
# refused rather than ignored - is unchanged.
grep -Fq 'if (language_seen) {' \
    kernel/src/appliance_config_validation.c || {
    echo "ITEM-509 inc2: третья строка записи не отвергается" >&2
    exit 1
}
grep -Fq 'appliance_config_validation_publish_language(' \
    kernel/src/appliance_config_validation.c || {
    echo "ITEM-509 inc2: публикация языка не живёт в validation" >&2
    exit 1
}

# 2. The durable record: the language line is written only when the language is
#    not the default, a hostname change keeps the chosen language, and an
#    unknown language is refused without a write.
grep -Fq 'appliance_persistent_config_set_language(' \
    kernel/include/appliance_persistent_config.h || {
    echo "ITEM-509 inc2: установка языка не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE = 10' \
    kernel/include/appliance_persistent_config.h || {
    echo "ITEM-509 inc2: отказ хранилища по языку не назван" >&2
    exit 1
}
grep -Fq 'language != APPLIANCE_PERSISTENT_CONFIG_DEFAULT_LANGUAGE' \
    kernel/src/appliance_persistent_config.c || {
    echo "ITEM-509 inc2: язык по умолчанию пишется в запись" >&2
    exit 1
}
# ITEM-516 passes the after-boot policy beside the language; the subject of
# this pin - a hostname change keeps the chosen language - is unchanged.
grep -Fq 'hostname, hostname_length, config->values.language,' \
    kernel/src/appliance_persistent_config.c || {
    echo "ITEM-509 inc2: смена hostname теряет выбранный язык" >&2
    exit 1
}

# 3. The integrity catalogue names the validated key, so the language is not a
#    quiet setting outside the validation contour.
# ITEM-516 added its own key to the same catalogue, so the count moved to 11.
grep -Fq '#define APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT 11U' \
    kernel/include/appliance_config_integrity.h || {
    echo "ITEM-509 inc2: каталог целостности не пересчитан" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_INTEGRITY_KIND_SYSTEM_LANGUAGE = 10' \
    kernel/include/appliance_config_integrity.h || {
    echo "ITEM-509 inc2: вид записи языка не объявлен" >&2
    exit 1
}
grep -Fq '"validated interface language"' \
    kernel/src/appliance_config_integrity.c || {
    echo "ITEM-509 inc2: запись языка не описана" >&2
    exit 1
}

# 4. The proofs drive the round trip, the old-format record and the refusals.
grep -Fq 'an old-format record keeps its hostname and reads as English' \
    tests/appliance-persistent-config-host.c || {
    echo "ITEM-509 inc2: проба не читает запись прежнего формата" >&2
    exit 1
}
grep -Fq 'returning to English writes the record of the previous format' \
    tests/appliance-persistent-config-host.c || {
    echo "ITEM-509 inc2: проба не проверяет возврат к прежнему формату" >&2
    exit 1
}
grep -Fq 'hostname-only record reads as the default language' \
    tests/appliance-config-validation-host.c || {
    echo "ITEM-509 inc2: проба не читает hostname-запись как en" >&2
    exit 1
}
grep -Fq 'validation refuses a language the catalog does not name' \
    tests/appliance-config-validation-host.c || {
    echo "ITEM-509 inc2: проба не отвергает чужой язык" >&2
    exit 1
}
grep -Fq 'language record key classifies to system config' \
    tests/appliance-config-integrity-host.c || {
    echo "ITEM-509 inc2: проба не классифицирует ключ языка" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_language.c \' Makefile || {
    echo "ITEM-509 inc2: пробы не линкуют каталог" >&2
    exit 1
}

# 5. Increment 4 is the command step this boundary waited for: the console now
#    reads the setting.  What must stay true is the storage boundary the
#    increment established - the console never opens the system record itself.
#    The language reaches it only through the value the boot published
#    (`console_publish_language()`) and the record owner's own write behind
#    `console_language_binding`; the adapter that touches the store lives in
#    kernel.c, and both the binding and the adapter are pinned by
#    `scripts/check.d/168-item-509-console-language.sh`.
for item509_inc2_store_call in \
    'appliance_persistent_config_language(' \
    'appliance_persistent_config_set_language(' \
    '#include "appliance_persistent_config.h"' \
    'APPLIANCE_CONFIG_VALIDATION_LANGUAGE_PREFIX' \
    'APPLIANCE_LANGUAGE_SETTING_KEY'; do
    if grep -Fq -- "$item509_inc2_store_call" kernel/src/console.c; then
        echo "ITEM-509 inc2: консоль открывает системную запись сама" >&2
        exit 1
    fi
done
for item509_inc2_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 2' "$item509_inc2_doc" || {
        echo "ITEM-509 inc2: $item509_inc2_doc не называет инкремент" >&2
        exit 1
    }
done
