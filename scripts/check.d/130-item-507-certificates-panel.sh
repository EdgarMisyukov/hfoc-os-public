#!/usr/bin/env bash
# ITEM-507 increment 5: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The panel manages the name -> certificate/key declarations.
#
# Increment 4 gave the mutation its scriptable target; this increment adds the
# twelfth section, its page and its form target.  The claims pinned here:
#
#   * `Certificates` is section index 11, appended after Account so every
#     earlier index and the `page = section + 1` mapping do not move; the
#     section count is twelve and the licenses page moved with it;
#   * the page lists the active table's summary (names, default, source) next
#     to the saved table's bounded `name (bytes)` list, carries the import and
#     delete/confirmation forms, says `applies=reboot`, never renders key or
#     certificate bytes, and a table that does not fit is cut at a complete
#     entry and flagged;
#   * `/panel/certificates` resolves to `CERTIFICATE_NAME` on POST and to the
#     rendered page on GET, and the panel target list names it;
#   * the serving path publishes the two renderings for the page, and the QEMU
#     proof reads the page on the live listener and requires the saved table
#     and both forms;
#   * every budget the new navigation link grew is re-measured and recorded,
#     and the fixtures that pin exact page bodies and the canonical menu moved
#     with it.
for item507_inc5_file in \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/src/appliance_production_serving.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    tests/https-handler-host.c \
    scripts/test-qemu-https-response-matrix.sh \
    scripts/test-qemu-production-service-https.sh \
    docs/hfoc-management-web-ui.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item507_inc5_file ]] || {
        echo "ITEM-507 inc5: отсутствует $item507_inc5_file" >&2
        exit 1
    }
done

# The section model: twelve sections, the new one appended, the page indices
# moved only for the licenses page that follows the sections (ITEM-508
# increment 34 appended the ACME control page after the licenses page, so the
# count moved 14 -> 15 while every section index stayed where this item put it).
for item507_inc5_section in \
    'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_COUNT 13U' \
    'APPLIANCE_MANAGEMENT_WEB_UI_SECTION_CERTIFICATES = 11,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_CERTIFICATES = 12,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_LICENSES = 14,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_COUNT = 17,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_CERTIFICATES 3123U' \
    'struct appliance_management_web_ui_certificates {'; do
    grep -Fq -- "$item507_inc5_section" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-507 inc5: модель панели не знает раздел:" \
            "$item507_inc5_section" >&2
        exit 1
    }
done
for item507_inc5_webui in \
    '"panel.section.certificates",' \
    '"/panel/certificates",' \
    'appliance_management_web_ui_write_certificates(' \
    'appliance_management_web_ui_write_certificates_form(' \
    'action=\"/panel/certificates\"' \
    'name=\"certificate\"' \
    '"panel.certificates.active_names"' \
    'applies=' \
    'PRIVATE KEY'; do
    if [[ $item507_inc5_webui == 'PRIVATE KEY' ]]; then
        if grep -Fq -- "$item507_inc5_webui" \
                kernel/src/appliance_management_web_ui.c; then
            echo "ITEM-507 inc5: страница рендерит материал ключа" >&2
            exit 1
        fi
        continue
    fi
    grep -Fq -- "$item507_inc5_webui" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-507 inc5: страница не несёт:" \
            "$item507_inc5_webui" >&2
        exit 1
    }
done
# ITEM-509 increment 10: the label is the catalog's now; the canonical English
# rendering is documented verbatim in the writer, so this pin still holds.
grep -Fq 'Recovery Routing Account Certificates Help</nav>' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-507 inc5: навигация не называет раздел" >&2
    exit 1
}

# The panel module: the view, its setter and the render call.
for item507_inc5_panel in \
    'struct appliance_management_web_ui_certificates certificates;' \
    'bool certificates_published;' \
    'appliance_management_panel_set_certificates(' \
    'appliance_management_panel_clear_certificates(' \
    'panel->certificates_published ? &panel->certificates : NULL,'; do
    grep -Fq -- "$item507_inc5_panel" \
        kernel/include/appliance_management_panel.h \
        kernel/src/appliance_management_panel.c || {
        echo "ITEM-507 inc5: панель не публикует раздел:" \
            "$item507_inc5_panel" >&2
        exit 1
    }
done

# The authorization and the HTTP policy: the panel target is the same mutation
# on POST and the rendered page on GET.
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-507 inc5: авторизация не знает панельную цель" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_CERTIFICATES_PATH,' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-507 inc5: панельная цель не разрешается" >&2
    exit 1
}
grep -Fq 'HTTP_POLICY_PANEL_CERTIFICATES_TARGET' \
    kernel/include/http_parser.h kernel/src/http_parser.c || {
    echo "ITEM-507 inc5: HTTP-политика не знает панельную цель" >&2
    exit 1
}

# The serving path: the two renderings and the page publication on every panel
# dispatch.
grep -Fq 'appliance_production_serving_publish_certificates(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-507 inc5: обслуживание не публикует страницу имён" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_certificates_render(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-507 inc5: таблицы не рендерятся" >&2
    exit 1
}

# The proofs: the page, the panel view and the fixtures that pin exact bodies.
for item507_inc5_webui_proof in \
    'management web UI Certificates section publishes the name ' \
    'management panel Certificates page shows both readings and never a '; do
    grep -Fq -- "$item507_inc5_webui_proof" \
        tests/appliance-management-web-ui-host.c \
        tests/appliance-management-panel-host.c || {
        echo "ITEM-507 inc5: проба не утверждает:" \
            "$item507_inc5_webui_proof" >&2
        exit 1
    }
done
grep -Fq '"/panel/certificates",' tests/appliance-management-panel-host.c || {
    echo "ITEM-507 inc5: проба панели не знает цель" >&2
    exit 1
}
grep -Fq '/panel/certificates' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-507 inc5: канонический список меню не знает раздел" >&2
    exit 1
}
grep -Fq 'Content-Length: 1611' tests/https-handler-host.c || {
    echo "ITEM-507 inc5: точное тело входа не перебазировано" >&2
    exit 1
}
for item507_inc5_qemu in \
    'the certificates page returned' \
    'the certificates page is missing' \
    'the certificates page rendered key material'; do
    grep -Fq -- "$item507_inc5_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-507 inc5: QEMU-проба не требует:" \
            "$item507_inc5_qemu" >&2
            exit 1
        }
done

# The documents: the measured row and the increment.
grep -Fq '| certificates | 3123 | 3123 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-507 inc5: документ панели не называет измеренный бюджет" >&2
    exit 1
}
grep -Fq 'ITEM-507 increment 5' docs/ARCHITECTURE.md docs/ROADMAP.md || {
    echo "ITEM-507 inc5: документы не записывают инкремент" >&2
    exit 1
}
