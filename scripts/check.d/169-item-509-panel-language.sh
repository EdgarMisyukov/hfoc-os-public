#!/usr/bin/env bash
# ITEM-509 increment 5: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 5 is the first half of 509.4: the panel tells the truth
# about the interface language.  The System page shows the active and the saved
# language from the same system record the hostname comes from, the owner reads
# the record at render time and publishes UNAVAILABLE rather than guessing, and
# both readings are escaped like every other value the page shows.
#
# The switch (form plus the two-phase stage -> token -> commit mutation with its
# audit) and `<html lang>` are the next increment, and the reason is pinned
# here as well: `lang` describes the language of the content, and the panel's
# text stays English until 509.7, so `lang="ru"` on an English page would be a
# lie.  The order is recorded in the roadmap, the architecture document, the
# surface inventory and the queue.
for item509_inc5_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/src/appliance_production_serving.c \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc5_file ]] || {
        echo "ITEM-509 inc5: отсутствует $item509_inc5_file" >&2
        exit 1
    }
done

# 1. The view carries the pair, and the page renders it (escaped, like every
#    other reading).
for item509_inc5_view in \
    'const char *language_active;' \
    'const char *language_saved;'; do
    grep -Fq -- "$item509_inc5_view" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-509 inc5: view-структура не несёт: $item509_inc5_view" >&2
        exit 1
    }
done
for item509_inc5_render in \
    '"panel.system.language_active"' \
    'system->language_active != NULL' \
    'system->language_active' \
    'system->language_saved' \
    '"</code></p>\n"'; do
    grep -Fq -- "$item509_inc5_render" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc5: страница не печатает: $item509_inc5_render" >&2
        exit 1
    }
done
# The pair is escaped through the same value writer as every other reading.
python3 - <<'PY' || {
import pathlib
import sys
text = pathlib.Path("kernel/src/appliance_management_web_ui.c").read_text(
    encoding="utf-8")
start = text.index('"panel.system.language_active"')
block = text[start:start + 1200]
for needle in ("appliance_management_web_ui_buffer_value(\n            buffer, system->language_active",
               "appliance_management_web_ui_buffer_value(\n            buffer, system->language_saved"):
    if needle not in block:
        print("ITEM-509 inc5: язык печатается без экранирования", file=sys.stderr)
        raise SystemExit(1)
PY
    echo "ITEM-509 inc5: язык печатается без экранирования" >&2
    exit 1
}

# 2. The owner reads the record at render time and publishes UNAVAILABLE when it
#    cannot be read - never a guessed language.
for item509_inc5_owner in \
    'appliance_production_serving_system_render_language' \
    'char system_language_active_text[16];' \
    'char system_language_saved_text[16];' \
    'appliance_persistent_config_language(store, &language) !=' \
    'memcpy(serving->system_language_active_text, "UNAVAILABLE",' \
    'system.language_active = serving->system_language_active_text;' \
    'system.language_saved = serving->system_language_saved_text;'; do
    grep -Fq -- "$item509_inc5_owner" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-509 inc5: владелец не несёт: $item509_inc5_owner" >&2
        exit 1
    }
done

# 3. The measured price is re-baselined in the same commit, not loosened: the
#    System page grew because of the pair (1857 -> 1939), and it moved again to
#    1962 when increment 6 added the language field to the Stage form, so the
#    header, the documented table and the ITEM-505 slices that pin both carry
#    the current measurement.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-509 inc5: бюджет System-страницы не пере-базирован" >&2
    exit 1
}
grep -Fq '| system | 2917 | 2917 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc5: таблица бюджетов не называет новую цену" >&2
    exit 1
}
for item509_inc5_slice in \
    scripts/check.d/117-item-505-live-panel-view.sh \
    scripts/check.d/118-item-505-system-hostname.sh; do
    grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U' \
        "$item509_inc5_slice" || {
        echo "ITEM-509 inc5: $item509_inc5_slice не пинит новую цену" >&2
        exit 1
    }
done

# 4. The host proof drives the page: the pair is shown, an unreadable record is
#    named, and the escaping checks still pass.
for item509_inc5_proof in \
    'system.language_active = "ru";' \
    'Language active <code>ru</code>' \
    'Language active <code>UNAVAILABLE</code>' \
    'management web UI System page shows both readings, the language and '; do
    grep -Fq -- "$item509_inc5_proof" \
        tests/appliance-management-web-ui-host.c || {
        echo "ITEM-509 inc5: host-проба не требует: $item509_inc5_proof" >&2
        exit 1
    }
done

# 5. The documents record the increment and the split that keeps `lang`
#    truthful.
for item509_inc5_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 5' "$item509_inc5_doc" || {
        echo "ITEM-509 inc5: $item509_inc5_doc не называет инкремент" >&2
        exit 1
    }
done
grep -Fq 'lang="ru"' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc5: инвентарь поверхностей не объясняет порядок lang" >&2
    exit 1
}
