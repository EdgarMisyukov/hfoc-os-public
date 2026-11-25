#!/usr/bin/env bash
# ITEM-509 increment 25: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 25 (509.7d-3c-1): the two Help prose fragments the panel
# still printed as English literals - the way back on a topic page and the
# sentence that introduces the index - read the one catalog in the machine's
# language.  The sentence is split around the fixed `<code>help</code>` markup,
# so each language orders its own words while the markup stays a literal, and
# the English document is byte for byte what it was.  The recorded Help budget
# moved with the longer Russian label, together with the documentation row.
for item509_inc25_file in \
    kernel/src/appliance_management_web_ui.c \
    kernel/src/appliance_language.c \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-language-host.c \
    scripts/check.d/187-item-509-console-refused.sh \
    docs/hfoc-language-surfaces.md \
    docs/hfoc-management-web-ui.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc25_file ]] || {
        echo "ITEM-509 inc25: отсутствует $item509_inc25_file" >&2
        exit 1
    }
done

# 1. The catalog carries the three entries, English verbatim (the spaces are
#    part of the index sentence: it joins the decimal count and the markup).
grep -Fq '{"panel.help.all_topics", "All help topics", "Все разделы справки"},' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc25: каталог не несёт подпись ссылки" >&2
    exit 1
}
grep -Fq '{"panel.help.index_intro",' kernel/src/appliance_language.c || {
    echo "ITEM-509 inc25: каталог не несёт начало вводной строки" >&2
    exit 1
}
grep -Fq '" topics from the built-in catalog; each one opens the same text the "' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc25: английское начало вводной строки не дословно" >&2
    exit 1
}
grep -Fq '"console'"'"'s ",' kernel/src/appliance_language.c || {
    echo "ITEM-509 inc25: английский хвост начала вводной строки не дословен" >&2
    exit 1
}
grep -Fq '{"panel.help.index_intro_tail", " command prints.", " в консоли."},' \
    kernel/src/appliance_language.c || {
    echo "ITEM-509 inc25: каталог не несёт хвост вводной строки" >&2
    exit 1
}
for item509_inc25_ru in \
    'Все разделы справки' \
    'разделов встроенного каталога; каждый открывает тот же текст, что и '; do
    grep -Fq -- "$item509_inc25_ru" kernel/src/appliance_language.c || {
        echo "ITEM-509 inc25: каталог не несёт русское значение: $item509_inc25_ru" >&2
        exit 1
    }
done

# 2. The panel reads the three values through the one catalog helper, and the
#    English literals are gone; the markup around them stays a literal.
for item509_inc25_call in \
    'buffer, language, "panel.help.all_topics");' \
    'buffer, language, "panel.help.index_intro");' \
    'buffer, language, "panel.help.index_intro_tail");'; do
    grep -Fq -- "$item509_inc25_call" kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc25: панель не читает каталог: $item509_inc25_call" >&2
        exit 1
    }
done
grep -Fq 'appliance_management_web_ui_buffer_language(' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc25: нет помощника каталога панели" >&2
    exit 1
}
grep -Fq '"<code>help</code>"' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-509 inc25: разметка команды потеряна" >&2
    exit 1
}
for item509_inc25_gone in \
    'All help topics</a>' \
    'topics from the built-in catalog'; do
    if grep -Fq -- "$item509_inc25_gone" kernel/src/appliance_management_web_ui.c; then
        echo "ITEM-509 inc25: английская проза осталась литералом:" \
            "$item509_inc25_gone" >&2
        exit 1
    fi
done

# 3. The Help budget was re-measured with the longer Russian label, and the
#    header constant and the documented row moved together.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_HELP 3650U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-509 inc25: бюджет Help не перемерен" >&2
    exit 1
}
grep -Fq '| help | 3650 | 3650 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc25: строка таблицы бюджетов не сдвинута" >&2
    exit 1
}

# 3b. The older slices that carried the previous value are re-pinned in the same
#     commit, so no contract still names the old Help budget.
for item509_inc25_repin in \
    scripts/check.d/172-item-509-panel-help.sh \
    scripts/check.d/174-item-509-panel-language.sh \
    scripts/check.d/175-item-509-panel-read-pages.sh; do
    grep -Fq 'help | 3650 | 3650 |' "$item509_inc25_repin" || {
        echo "ITEM-509 inc25: срез не пере-пинен на новый бюджет: $item509_inc25_repin" >&2
        exit 1
    }
done
if grep -Fq 'BUDGET_HELP 3460U' \
    scripts/check.d/172-item-509-panel-help.sh \
    scripts/check.d/175-item-509-panel-read-pages.sh; then
    echo "ITEM-509 inc25: старый бюджет Help остался в срезах" >&2
    exit 1
fi

# 4. The proof drives both fragments in both languages and keeps the English
#    document exactly as it was.
for item509_inc25_check in \
    'the Help index explains itself in the chosen language' \
    'the English Help index sentence is gone from a ' \
    'the Help topic page is a complete document with a way ' \
    'the English way back is gone from a translated '; do
    grep -Fq -- "$item509_inc25_check" tests/appliance-management-web-ui-host.c || {
        echo "ITEM-509 inc25: проба панели не утверждает: $item509_inc25_check" >&2
        exit 1
    }
done
grep -Fq 'panel.help.all_topics' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc25: проба панели не читает подпись" >&2
    exit 1
}
grep -Fq '330 measured' tests/appliance-language-host.c || {
    echo "ITEM-509 inc25: проба каталога не называет измеренное число" >&2
    exit 1
}
grep -Fq '330 measured' scripts/check.d/187-item-509-console-refused.sh || {
    echo "ITEM-509 inc25: срез 187 не пере-пинен на новое число" >&2
    exit 1
}

# 5. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 25' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc25: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '3460 -> 3481' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc25: документ панели не объясняет перемер" >&2
    exit 1
}
grep -Fq '509.7d-3c-1' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc25: очередь не называет кусок 509.7d" >&2
    exit 1
}
