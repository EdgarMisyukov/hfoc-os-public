#!/usr/bin/env bash
# ITEM-508 increment 37: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The read-only ACME block shows the order path's own progress.
#
# Point 10 asks the panel for "the last attempt and its result"; increments
# 32/34 published the durable state and the control, and 35/36 added the order
# path, but its progress lived only in the serial record.  This increment adds
# one bounded value (`STATE/STEP`, e.g. `RUNNING/ORDER`, `DONE/CERTIFICATE`,
# `FAILED/CHALLENGE`) to the read-only block: no form, no mutation, no promise
# about the CA, and the Dashboard budget re-measured with it.
for item508_inc37_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_production_acme_order_boot.h \
    kernel/src/appliance_production_acme_order_boot.c \
    kernel/include/appliance_production_acme_order.h \
    kernel/src/appliance_production_acme_order.c \
    kernel/src/kernel.c \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc37_file ]] || {
        echo "ITEM-508 inc37: отсутствует $item508_inc37_file" >&2
        exit 1
    }
done

# 1. The view value and the renderer: one bounded field, rendered inside the
#    read-only paragraph.
grep -Fq 'const char *order;' kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-508 inc37: вид ACME не несёт значение заказа" >&2
    exit 1
}
grep -Fq '"panel.acme.order"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-508 inc37: блок ACME не печатает заказ" >&2
    exit 1
}
grep -Fq 'buffer, acme->order,' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-508 inc37: заказ не ограничен при печати" >&2
    exit 1
}
grep -Fq 'view.order = appliance_production_acme_order_boot_state();' \
    kernel/src/kernel.c || {
    echo "ITEM-508 inc37: обычная загрузка не публикует заказ в панель" >&2
    exit 1
}

# 2. The sources: the driver's stage is public, and the boot composition
#    renders one bounded `STATE/STEP`.
grep -Fq 'appliance_production_acme_order_stage_string' \
    kernel/include/appliance_production_acme_order.h || {
    echo "ITEM-508 inc37: ступень драйвера не опубликована" >&2
    exit 1
}
grep -Fq 'appliance_production_acme_order_stage(' \
    kernel/include/appliance_production_acme_order.h || {
    echo "ITEM-508 inc37: геттер ступени драйвера не объявлен" >&2
    exit 1
}
grep -Fq 'appliance_production_acme_order_boot_state' \
    kernel/include/appliance_production_acme_order_boot.h || {
    echo "ITEM-508 inc37: композиция не публикует состояние заказа" >&2
    exit 1
}
grep -Fq 'state_text' kernel/src/appliance_production_acme_order_boot.c || {
    echo "ITEM-508 inc37: композиция не собирает ограниченную строку" >&2
    exit 1
}

# 3. The proof: the block names the order value, escapes a hostile one and the
#    Dashboard budget moved with it.
grep -Fq 'certificates.acme.order = "RUNNING/ORDER";' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-508 inc37: проба не подставляет заказ" >&2
    exit 1
}
grep -Fq 'strstr(block, "order RUNNING/ORDER") != NULL' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-508 inc37: проба не требует заказ в блоке" >&2
    exit 1
}
grep -Fq 'certificates.acme.order = longest_acme;' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-508 inc37: бюджет не измерен с заказом" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DASHBOARD 2759U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-508 inc37: бюджет Dashboard не перебазирован" >&2
    exit 1
}
grep -Fq '| dashboard | 2759 | 2759 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-508 inc37: документация не несёт строку бюджета" >&2
    exit 1
}
for item508_inc37_pin in \
    scripts/check.d/117-item-505-live-panel-view.sh \
    scripts/check.d/159-item-508-acme-panel-state.sh; do
    grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DASHBOARD 2759U' \
        "$item508_inc37_pin" || {
        echo "ITEM-508 inc37: пин бюджета не сдвинут в $item508_inc37_pin" >&2
        exit 1
    }
done

# 4. The documents name the increment.
for item508_inc37_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 37' "$item508_inc37_doc" || {
        echo "ITEM-508 inc37: $item508_inc37_doc не называет инкремент" >&2
        exit 1
    }
done
