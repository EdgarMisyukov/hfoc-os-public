#!/usr/bin/env bash
# ITEM-647: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The owner asked for one visible, associated label per visible panel control,
# in the selected language only, and for a working browser remove form.  This
# slice pins the shared catalog keys, the labelled controls, the hidden fixed
# action fields, the two route stage forms (add and remove with the right
# field set), the parser's refusal of the old body and the host proofs.
# shellcheck disable=SC2016

item647_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item647_root" || exit 1

for item647_file in \
    kernel/src/appliance_language.c \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-route-config-host.c \
    tests/appliance-language-host.c \
    docs/hfoc-management-web-ui.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item647_file ]] || {
        echo "ITEM-647: отсутствует $item647_file" >&2
        exit 1
    }
done

# 1. The shared catalog carries the one confirmation term and the field names.
for item647_key in \
    '{"panel.form.confirm_code", "Confirmation code", "Код подтверждения"}' \
    '{"panel.form.confirm_hint", "Appears after the change is staged.",' \
    '{"panel.form.hostname", "Hostname", "Имя устройства"}' \
    '{"panel.form.route_name", "Route name", "Имя маршрута"}' \
    '{"panel.form.backend_address", "Server IPv4", "IPv4 сервера"}' \
    '{"panel.form.tls_to_server", "TLS to server (off for HTTP)",' \
    '{"panel.form.route_remove", "Remove route", "Удалить маршрут"}'; do
    grep -Fq -- "$item647_key" kernel/src/appliance_language.c || {
        echo "ITEM-647: нет ключа $item647_key" >&2
        exit 1
    }
done

# 2. One helper renders the labelled confirmation field with its hint.
grep -Fq 'appliance_management_web_ui_write_token_field' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-647: нет общего поля подтверждения" >&2
    exit 1
}
grep -Fq '" <input name=\"token\" inputmode=\"numeric\"></label><br>"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-647: поле подтверждения не подписано" >&2
    exit 1
}

# 3. The fixed protocol actions are hidden, and both route stage forms exist.
for item647_action in import delete add remove; do
    item647_pattern='<input type=\"hidden\" name=\"action\" value=\"'"$item647_action"'\">'
    grep -Fq -- "$item647_pattern" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-647: action=$item647_action не скрыт" >&2
        exit 1
    }
done
grep -Fq '"panel.form.route_name"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-647: поле имени маршрута не подписано" >&2
    exit 1
}
grep -Fq '" <select name=\"tls\">' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-647: TLS до сервера не подписан" >&2
    exit 1
}
grep -Fq '"panel.form.confirm_code"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-647: код подтверждения не подписан" >&2
    exit 1
}
grep -Fq '<input name=\"token\" inputmode=\"numeric\"></label>' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-647: поле кода подтверждения не связано с label" >&2
    exit 1
}
# The add-only fields appear once each: only the add form carries them.
for item647_field in backend port tls; do
    [[ $(grep -Fc "name=\\\"$item647_field\\\"" \
        kernel/src/appliance_management_web_ui.c) -eq 1 ]] || {
        echo "ITEM-647: поле $item647_field встречается не один раз" >&2
        exit 1
    }
done

# 4. The host proof audits nesting and both languages, and the parser keeps the
#    old empty-extra body refused.
grep -Fq 'check_visible_controls_labelled' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-647: host-проба не проверяет вложенность label" >&2
    exit 1
}
grep -Fq 'the old remove body with empty extra fields stays refused' \
    tests/appliance-route-config-host.c || {
    echo "ITEM-647: старый remove-body не отвергается" >&2
    exit 1
}
grep -Fq 'CHECK(count <= 448U, "catalog stays a small table (%zu)", count);' \
    tests/appliance-language-host.c || {
    echo "ITEM-647: потолок каталога не поднят осознанно" >&2
    exit 1
}
grep -Fq 'for label in ("Имя устройства", "Язык панели", "Код подтверждения"):' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-647: QEMU-проба не читает русские подписи" >&2
    exit 1
}

# 5. The measured page budgets and their documentation moved with the labels.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ROUTING 3532U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-647: бюджет routing не обновлён" >&2
    exit 1
}
grep -Fq '| routing | 3532 | 3532 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-647: таблица бюджетов не обновлена" >&2
    exit 1
}

exit 0
