#!/usr/bin/env bash
# ITEM-499: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The panel menu leads to the pages the machine serves.
#
# The owner opened the panel over the network and every menu entry answered
# `404 Not Found` / `not_found`, while the page itself rendered.  The cause was
# in the writer, not in the routing: `appliance_management_web_ui_write_nav`
# printed `<a href="/panel` and then appended the section's *absolute* path from
# `appliance_management_web_ui_section_paths[]`, so Network was published as
# `/panel//panel/network` - a target the router does not match.
#
# What this slice pins:
#
#   * there is one source of truth for a section target: the writer takes the
#     href from the same accessor the panel router matches a request with, and
#     it never assembles a path from a prefix plus a path;
#   * the host proof reads every rendered document, extracts every published
#     `href` and form `action` and requires each target to be one the machine
#     serves - it fails on the tree that shipped the bug;
#   * the QEMU proof walks the links of the served document as a remote client:
#     every target answers the sign-in document anonymously (401, never 404) and
#     its own page with the session;
#   * an `UNAVAILABLE` section names why nothing is published for it, instead of
#     leaving the reader at the word alone;
#   * the re-baselined page budgets are recorded in the header and repeated in
#     the document, so the shrink the fix caused cannot drift silently.

for item499_file in \
    kernel/src/appliance_management_web_ui.c \
    kernel/src/appliance_management_panel.c \
    kernel/include/appliance_management_web_ui.h \
    tests/appliance-management-web-ui-host.c \
    scripts/test-qemu-https-response-matrix.sh \
    docs/hfoc-management-web-ui.md; do
    test -f "$item499_file" || {
        echo "ITEM-499: отсутствует файл: $item499_file" >&2
        exit 1
    }
done

# ------------------------------------------ one source of truth for a target --
# The writer resolves the path through the same accessor the router uses; the
# buggy prefix concatenation cannot come back without deleting this call.
grep -Fq 'appliance_management_web_ui_section_path(' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-499: меню панели не берёт цель из объявленного пути" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_section_path(' \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-499: маршрутизатор панели не сверяется с объявленным путём" >&2
    exit 1
}
grep -Fq '"<a href=\"' kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-499: writer не печатает href отдельным атрибутом" >&2
    exit 1
}
if grep -Fq '<a href="/panel' kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-499: writer снова склеивает префикс /panel с полным путём" >&2
    exit 1
fi
# The `href` attribute is built by the writer from the accessor, and the label
# comes from the one catalog through the section key table: neither is
# re-derived from the other.  ITEM-509 increment 10 consciously moved this pin:
# the module's own `section_names[]` table was a second source of truth for the
# same thirteen words, so the label is now
# `appliance_management_web_ui_buffer_language(... section_keys[index])`.
grep -Fq 'appliance_management_web_ui_section_keys[index]);' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-499: меню панели потеряло имя раздела" >&2
    exit 1
}
if grep -Fq 'appliance_management_web_ui_section_names' \
    kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-499: у меню панели снова два источника имён разделов" >&2
    exit 1
fi

# ------------------------------------------------ the host proof reads links --
grep -Fq 'test_link_integrity' tests/appliance-management-web-ui-host.c || {
    echo "ITEM-499: host-проба целостности ссылок отсутствует" >&2
    exit 1
}
for item499_host_literal in \
    'management web UI menu reaches every section' \
    'management web UI menu publishes one link per served target' \
    'is one the machine serves' \
    'no read side is published'; do
    grep -Fq "$item499_host_literal" \
        tests/appliance-management-web-ui-host.c || {
        echo "ITEM-499: host-проба не утверждает: $item499_host_literal" >&2
        exit 1
    }
done
grep -Fq 'check_published_targets(page, "action=\"", true)' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-499: host-проба не обходит цели форм" >&2
    exit 1
}

# ------------------------------------------- the QEMU proof walks the menu ----
for item499_qemu_literal in \
    'canonical_nav_targets' \
    'published_nav_targets' \
    'the published menu does not name the served targets' \
    'the menu joins a target twice'; do
    grep -Fq "$item499_qemu_literal" \
        scripts/test-qemu-https-response-matrix.sh || {
        echo "ITEM-499: QEMU-проба не обходит меню: $item499_qemu_literal" >&2
        exit 1
    }
done
grep -Fq 're.findall(' \
    scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-499: QEMU-проба не читает ссылки из отданного документа" >&2
    exit 1
}

# ------------------------------------------- the reasons and the re-baseline --
# ITEM-509 increment 11 moved the UNAVAILABLE prose into the string catalog:
# the renderer names the key, the catalog carries the English words and the
# Russian beside them.
grep -Fq '"panel.state.unavailable"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-499: UNAVAILABLE не называет причину" >&2
    exit 1
}
grep -Fq 'No management API owner is bound to this section, so no read side is ' \
    kernel/src/appliance_language.c || {
    echo "ITEM-499: UNAVAILABLE не называет причину" >&2
    exit 1
}
# ITEM-504: the menu gained the Routing link, so every section page and the
# licenses page grew by that link and was re-baselined with the header.
# ITEM-506 increment 6 gained the Account link the same way; the licenses page
# is 1964 bytes after the trailing-stage sentence was dropped to stay inside the
# ceiling, so the pin below moved with the measurement.
grep -Fq '#define APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES 2736U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-504: бюджет licenses page не перебазирован" >&2
    exit 1
}
grep -Fq 'licenses | 2736 | 2736 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-499: документ не повторяет перебазированный бюджет" >&2
    exit 1
}
grep -Fq 'single source of truth' docs/hfoc-management-web-ui.md || {
    echo "ITEM-499: документ не записывает единственный источник правды" >&2
    exit 1
}
grep -Fq 'ITEM-499' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-499: очередь не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-499' docs/ROADMAP.md || {
    echo "ITEM-499: роадмап не называет пункт" >&2
    exit 1
}
