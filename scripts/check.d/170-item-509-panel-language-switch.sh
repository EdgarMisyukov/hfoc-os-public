#!/usr/bin/env bash
# ITEM-509 increment 6: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-509 increment 6 is the second half of 509.4: the panel changes the
# language.  The switch rides the two-phase system mutation the hostname already
# uses - one field per body, a token, a commit that writes the durable record -
# so the panel and the console command write the same system record and there is
# one source of truth.  The claims pinned here:
#
#   * the stage accepts `hostname=` or `language=`, and the language is parsed
#     by the catalog's own rule (exactly `en`/`ru`), so a typo is refused before
#     a token is issued and nothing is written without one;
#   * the commit writes the language through the same
#     `appliance_persistent_config_set_language()` the console command uses and
#     leaves its own English record naming the value, the persistence verdict
#     and that the change applies to the next request;
#   * no new authorization entry is needed - the route already resolves to the
#     system mutation - and the pin says so rather than leaving it implicit;
#   * the page's Stage form carries the language field - ITEM-513 later split
#     the one two-field form into one form per field, because the stage parses
#     exactly one field per body - and the System page budget is re-measured in
#     the same commit as the rendering it pays for;
#   * `<html lang>` stays out of this increment on purpose: the attribute
#     describes the language of the rendered document, and the panel's text is
#     still English until 509.7.
for item509_inc6_file in \
    kernel/src/appliance_production_serving.c \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc6_file ]] || {
        echo "ITEM-509 inc6: отсутствует $item509_inc6_file" >&2
        exit 1
    }
done

# 1. The stage: one field, either name, the catalog's parser for the language.
#    ITEM-515 moved the one-field shape into `appliance_panel_form_system_parse`
#    so the host proof can drive it; the stage still branches on the field the
#    parser names and validates the value the same way.
for item509_inc6_stage in \
    'language_staged;' \
    'enum appliance_language language;' \
    'appliance_panel_form_system_parse(' \
    'APPLIANCE_PANEL_FORM_SYSTEM_HOSTNAME' \
    'APPLIANCE_PANEL_FORM_SYSTEM_LANGUAGE' \
    'appliance_language_parse(' \
    'serving->system_candidate.language_staged = true;'; do
    grep -Fq -- "$item509_inc6_stage" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-509 inc6: stage не несёт: $item509_inc6_stage" >&2
        exit 1
    }
done
# The field reader takes the field's name, and the old hard-coded one is gone.
grep -Fq 'const char *name,' kernel/src/appliance_production_serving.c || {
    echo "ITEM-509 inc6: чтение поля не обобщено на имя" >&2
    exit 1
}
if grep -Fq 'static const char name[] = "hostname";' \
    kernel/src/appliance_production_serving.c; then
    echo "ITEM-509 inc6: чтение поля всё ещё жёстко знает hostname" >&2
    exit 1
fi

# 2. The commit writes the language through the shared record and leaves its own
#    audit line.
for item509_inc6_commit in \
    'appliance_persistent_config_set_language(' \
    'serving->system_candidate.language_staged' \
    'applies=next-request' \
    '"[PRODUCTION-SYSTEM-LANGUAGE] source=PANEL value="' \
    'appliance_production_serving_system_language_record(serving,'; do
    grep -Fq -- "$item509_inc6_commit" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-509 inc6: commit не несёт: $item509_inc6_commit" >&2
        exit 1
    }
done
# A record is only published with the candidate still in hand.
python3 - <<'ITEM509INC6' || {
import pathlib
import sys
text = pathlib.Path("kernel/src/appliance_production_serving.c").read_text(
    encoding="utf-8")
start = text.index("appliance_production_serving_system_language_record(serving,")
tail = text[start:start + 400]
if "memset(&serving->system_candidate" not in tail:
    print("ITEM-509 inc6: кандидат стирается до записи", file=sys.stderr)
    raise SystemExit(1)
ITEM509INC6
    echo "ITEM-509 inc6: запись языка не проверена" >&2
    exit 1
}

# 3. No new authorization entry: the system mutation already covers the route,
#    and the pin states the decision instead of leaving it implicit.
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_SYSTEM_HOSTNAME' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-509 inc6: маршрут системы не разрешён" >&2
    exit 1
}
if grep -Fq 'SYSTEM_LANGUAGE' \
    kernel/include/appliance_management_authorization.h; then
    echo "ITEM-509 inc6: добавлена лишняя запись авторизации" >&2
    exit 1
fi
grep -Fq 'policy.matrix.count == 17U' \
    tests/appliance-management-authorization-host.c || {
    echo "ITEM-509 inc6: счётчик политики неожиданно изменился" >&2
    exit 1
}

# 4. The stage form carries the language field, and the price is the
#    re-measured one.  ITEM-513 split the form the increment drew: the stage
#    parses exactly one field per body, so the drawn form is now one stage form
#    per field and the body a browser submits is the body the stage accepts.
for item509_inc6_form in \
    '" <input name=\"hostname\"></label>"' \
    '" <input name=\"language\"></label>"'; do
    grep -Fq -- "$item509_inc6_form" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-509 inc6: форма не несёт поле языка" >&2
        exit 1
    }
done
if grep -Fq '"<input name=\"hostname\"><input name=\"language\">"' \
    kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-509 inc6: форма всё ещё подаёт два поля одним телом" >&2
    exit 1
fi
grep -Fq 'name=\"language\"' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-509 inc6: host-проба не требует поле языка" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SYSTEM 2917U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-509 inc6: бюджет System-страницы не пере-базирован" >&2
    exit 1
}
grep -Fq '| system | 2917 | 2917 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-509 inc6: таблица бюджетов не называет новую цену" >&2
    exit 1
}

# 5. `<html lang>` stays out, and the documents say why.
if grep -Fq 'appliance_management_web_ui_set_language' \
    kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-509 inc6: lang включён раньше перевода строк" >&2
    exit 1
fi
for item509_inc6_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-language-surfaces.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-509 increment 6' "$item509_inc6_doc" || {
        echo "ITEM-509 inc6: $item509_inc6_doc не называет инкремент" >&2
        exit 1
    }
done
grep -Fq '509.7' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc6: инвентарь поверхностей не объясняет порядок lang" >&2
    exit 1
}
