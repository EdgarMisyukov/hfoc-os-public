#!/usr/bin/env bash
# ITEM-508 increment 32: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The panel shows the ACME state, read-only.
#
# ITEM-508 point 10: the panel shows whether ACME is on, the names, the expiry
# and the last attempt with its result - as a reading.  Enabling, disabling and
# starting an issuance stay two-phase operator mutations, so the block carries
# no form and no action.  The Certificates page is measured at its 2048-byte
# cap with two name tables and four forms, so the block lives on the Dashboard
# beside the generic view, and each of its values is rendered at its own bound
# so the page cannot overflow.
for item508_inc32_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    tests/appliance-management-web-ui-host.c \
    docs/hfoc-management-web-ui.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc32_file ]] || {
        echo "ITEM-508 inc32: отсутствует $item508_inc32_file" >&2
        exit 1
    }
done

# 1. The view: a named ACME struct nested in the certificates view, so no
#    existing render call site changes signature, and a bounded value size.
for item508_inc32_view_pin in \
    'struct appliance_management_web_ui_acme {' \
    'APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX 24U' \
    'struct appliance_management_web_ui_acme acme;' \
    'const char *renewal;' \
    'const char *expires;' \
    'const char *attempts;'; do
    grep -Fq -- "$item508_inc32_view_pin" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-508 inc32: вид не содержит «$item508_inc32_view_pin»" >&2
        exit 1
    }
done

# 2. The renderer: one block writer, rendered on the Dashboard, reading values
#    through the bounded renderer; the block is a paragraph and carries no form.
for item508_inc32_render_pin in \
    'static void appliance_management_web_ui_write_acme(' \
    'appliance_management_web_ui_buffer_value_bounded(' \
    'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_DASHBOARD' \
    'appliance_management_web_ui_write_acme(' \
    '"panel.acme.none_published"'; do
    grep -Fq -- "$item508_inc32_render_pin" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-508 inc32: рендерер не содержит «$item508_inc32_render_pin»" \
            >&2
        exit 1
    }
done
python3 - <<'PY' || exit 1
import pathlib

source = pathlib.Path("kernel/src/appliance_management_web_ui.c").read_text()
writer = source.find("static void appliance_management_web_ui_write_acme(")
if writer == -1:
    raise SystemExit("ITEM-508 inc32: блок ACME не найден")
# The block ends at the next top-level function after its body; the writer is
# small, so a bounded window is enough to see that it renders no form.
window = source[writer:writer + 2600]
if "<form" in window or "action=" in window:
    raise SystemExit("ITEM-508 inc32: блок ACME содержит форму или действие")
PY

# 3. The producer: the serving path stores the borrowed view and republishes
#    the page only when the view moved; the ordinary boot composes it from the
#    service's own getters and publishes it.
for item508_inc32_producer_pin in \
    'appliance_production_serving_publish_acme(' \
    'struct appliance_management_web_ui_acme acme;' \
    'appliance_production_serving_publish_certificates(serving)' \
    'NO_ACME_STATE'; do
    grep -Fq -- "$item508_inc32_producer_pin" \
        kernel/include/appliance_production_serving.h \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-508 inc32: проводка не содержит «$item508_inc32_producer_pin»" \
            >&2
        exit 1
    }
done
for item508_inc32_kernel_pin in \
    'static void publish_production_acme_panel_view(void)' \
    'appliance_production_acme_service_state_string(' \
    'appliance_acme_account_status_string(' \
    'appliance_acme_renewal_decision_string(' \
    'appliance_acme_renewal_expires_at(' \
    'appliance_acme_renewal_attempts(' \
    'appliance_production_serving_publish_acme(&view)' \
    'publish_production_acme_panel_view();'; do
    grep -Fq -- "$item508_inc32_kernel_pin" kernel/src/kernel.c || {
        echo "ITEM-508 inc32: kernel.c не содержит «$item508_inc32_kernel_pin»" \
            >&2
        exit 1
    }
done
# The block never carries key material: the composition reads state strings and
# the renewal policy's numbers, never the account record's thumbprint field.
python3 - <<'PY' || exit 1
import pathlib

source = pathlib.Path("kernel/src/kernel.c").read_text()
compose = source.find("static void publish_production_acme_panel_view(void)")
if compose == -1:
    raise SystemExit("ITEM-508 inc32: композиция блока ACME не найдена")
end = source.find("\n}\n", compose)
window = source[compose:end if end != -1 else compose + 2000]
if "thumbprint" in window or "record" in window:
    raise SystemExit("ITEM-508 inc32: блок ACME тянет запись аккаунта")
PY

# 4. The host proof drives the block: the whole state, no mutation, escaping,
#    and the named reason when nothing is published.
for item508_inc32_probe_pin in \
    'static void test_dashboard_acme_block(void)' \
    'the ACME block names the whole state the point asks for' \
    'the ACME block is a reading, not a mutation' \
    'the ACME block escapes a hostile value' \
    'an unpublished ACME block names its reason instead of vanishing'; do
    grep -Fq -- "$item508_inc32_probe_pin" \
        tests/appliance-management-web-ui-host.c || {
        echo "ITEM-508 inc32: проба не содержит «$item508_inc32_probe_pin»" >&2
        exit 1
    }
done

# 5. The measured budget and its recorded row moved together.  Increment 34
#    moved the Dashboard row again (1911 -> 1942) with the link to the control
#    page; ITEM-509 increment 11 re-measured it in both languages (2590).
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_DASHBOARD 2759U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-508 inc32: бюджет Dashboard не назван" >&2
    exit 1
}
grep -Fq '| dashboard | 2759 | 2759 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-508 inc32: docs/hfoc-management-web-ui.md не несёт строку бюджета" \
        >&2
    exit 1
}

# 6. The documents name the increment.
for item508_inc32_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 32' "$item508_inc32_doc" || {
        echo "ITEM-508 inc32: $item508_inc32_doc не называет инкремент" >&2
        exit 1
    }
done
