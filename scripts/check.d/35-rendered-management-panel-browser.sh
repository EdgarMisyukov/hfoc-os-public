#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# ITEM-384 rendered management panel and browser hardening contracts.
for web_ui_render_symbol in \
    appliance_management_web_ui_escape \
    appliance_management_web_ui_render_login \
    appliance_management_web_ui_render_section \
    appliance_management_web_ui_section_state \
    appliance_management_web_ui_section_path \
    appliance_management_web_ui_page_budget; do
    grep -Fq "$web_ui_render_symbol" \
        kernel/include/appliance_management_web_ui.h \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-384 web UI не публикует $web_ui_render_symbol" >&2;
        exit 1; }
done
for web_ui_state_literal in \
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_OWNED \
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_DEGRADED \
    APPLIANCE_MANAGEMENT_WEB_UI_SECTION_STATE_UNAVAILABLE; do
    grep -Fq "$web_ui_state_literal" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-384 web UI не публикует $web_ui_state_literal" >&2;
        exit 1; }
done
for web_ui_page_key in \
    '"panel.confirm.title"' \
    '"panel.confirm.single_use"' \
    '"panel.confirm.token"'; do
    grep -Fq "$web_ui_page_key" \
        kernel/src/appliance_management_web_ui.c || {
        echo "ITEM-384 web UI не публикует раздел или подтверждение" >&2;
        exit 1; }
done
# ITEM-509 increment 12 moved the confirmation prose into the catalog too; the
# renderer names the keys and the catalog carries the English words.
for web_ui_page_literal in \
    'Two-phase confirmation' \
    'single-use' \
    'Confirmation token: '; do
    grep -Fq "$web_ui_page_literal" \
        kernel/src/appliance_language.c || {
        echo "ITEM-384 web UI не публикует раздел или подтверждение" >&2;
        exit 1; }
done
# ITEM-509 increment 11 moved the unavailable-section prose into the catalog;
# the renderer now names the key and the catalog carries the English words.
grep -Fq '"panel.state.unavailable"'     kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-384 web UI не публикует недоступный раздел" >&2;
    exit 1; }
grep -Fq 'No management API owner is bound to this section'     kernel/src/appliance_language.c || {
    echo "ITEM-384 web UI не публикует недоступный раздел" >&2;
    exit 1; }
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LOGIN' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-384 web UI не публикует записанный бюджет страниц" >&2;
    exit 1; }
test -f docs/hfoc-management-web-ui.md || {
    echo "Отсутствует ITEM-384 management web UI document" >&2;
    exit 1; }
grep -Fq '| page | bytes | budget |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-384 document не записывает бюджет страниц" >&2;
    exit 1; }
grep -Fq 'escap' docs/hfoc-management-web-ui.md || {
    echo "ITEM-384 document не документирует escaping" >&2;
    exit 1; }
grep -Fq 'test-appliance-management-panel-host' Makefile || {
    echo "Makefile не содержит test-appliance-management-panel-host" >&2;
    exit 1; }
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-management-panel-host' Makefile || {
    echo "test-host-suite не содержит test-appliance-management-panel-host" >&2;
    exit 1; }
grep -Fq 'APPLIANCE_MANAGEMENT_PANEL_HOST_TEST' \
    kernel/src/appliance_management_panel.c || {
    echo "management panel не помечен APPLIANCE_MANAGEMENT_PANEL_HOST_TEST" >&2;
    exit 1; }
for panel_symbol in \
    appliance_management_panel_target \
    appliance_management_panel_login \
    appliance_management_panel_dispatch; do
    grep -Fq "$panel_symbol" \
        kernel/include/appliance_management_panel.h \
        kernel/src/appliance_management_panel.c || {
        echo "management panel не объявляет $panel_symbol" >&2;
        exit 1; }
done
grep -Fq 'appliance_production_auth_authorize_request' \
    kernel/src/appliance_management_panel.c || {
    echo "management panel не использует существующую матрицу авторизации" >&2;
    exit 1; }
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_management_panel.c; then
    echo "appliance_management_panel.c использует запрещённое выделение, диагностику, аудит или I/O" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_ROUTE_PANEL' \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c || {
    echo "management authorization не знает панель" >&2;
    exit 1; }
for hardening_header in \
    'Cache-Control: no-store' \
    'Strict-Transport-Security' \
    'Content-Security-Policy: default-src' \
    'Referrer-Policy: no-referrer' \
    'X-Content-Type-Options: nosniff' \
    'X-Frame-Options: DENY'; do
    grep -Fq "$hardening_header" \
        kernel/src/http_response.c \
        kernel/include/http_response.h \
        kernel/include/appliance_management_tls.h || {
        echo "management listener не публикует hardening header" >&2;
        exit 1; }
done
grep -Fq "Content-Security-Policy: default-src 'none'" \
    scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix не проверяет Content-Security-Policy" >&2;
    exit 1; }
grep -Fq 'panel_authorization' \
    scripts/test-qemu-https-response-matrix.sh || {
    echo "HTTPS response matrix не проверяет панель с сессией" >&2;
    exit 1; }
grep -Fq 'TCP_SND_BUF' kernel/include/lwipopts.h || {
    echo "фикстурный TCP send buffer не покрывает отрендеренную страницу" >&2;
    exit 1; }
grep -Fq 'TCP_SND_BUF' docs/hfoc-management-web-ui.md || {
    echo "ITEM-384 document не документирует транспортный предел страницы" >&2;
    exit 1; }
grep -Fq 'Authorization: Bearer' docs/hfoc-management-web-ui.md || {
    echo "ITEM-384 document не документирует сессионный заголовок" >&2;
    exit 1; }

# ITEM-382 licensing surface and notices closure contracts.
for license_surface_file in \
    release/hfoc-license-inventory.txt \
    notices/mbedtls-Apache-2.0-election.txt \
    scripts/verify-license-inventory.sh \
    scripts/verify-license-notices.sh \
    scripts/emit-license-sbom-rows.sh \
    kernel/include/appliance_license_inventory.h \
    kernel/src/appliance_license_inventory.c \
    tests/appliance-license-inventory-host.c \
    docs/hfoc-appliance-license-surface.md; do
    test -f "$license_surface_file" || {
        echo "Отсутствует ITEM-382 file: $license_surface_file" >&2
        exit 1
    }
done
for license_surface_script in \
    scripts/verify-license-inventory.sh \
    scripts/verify-license-notices.sh \
    scripts/emit-license-sbom-rows.sh; do
    test -x "$license_surface_script" || {
        echo "$license_surface_script не исполняемый" >&2
        exit 1
    }
    bash -n "$license_surface_script" || {
        echo "Синтаксис $license_surface_script некорректен" >&2
        exit 1
    }
done
grep -Fq 'test-appliance-license-inventory-host' Makefile || {
    echo "Makefile не содержит test-appliance-license-inventory-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-license-inventory-host' Makefile || {
    echo "test-host-suite не содержит test-appliance-license-inventory-host" >&2
    exit 1
}
# The declared inventory is the single source of truth: it states the appliance
# dual license, the mbedTLS election as Apache-2.0 (never GPL or "dual"), and
# the notice path of every bundled dependency.
bash scripts/verify-license-inventory.sh || {
    echo "declared license inventory не проходит собственную проверку" >&2
    exit 1
}
for license_inventory_literal in \
    'appliance_license=AGPL-3.0-only OR LicenseRef-HFOC-Commercial' \
    'dependency_count=4' \
    'dependency_2_id=mbedtls' \
    'dependency_2_license=Apache-2.0' \
    'dependency_2_license_offered=Apache-2.0 OR GPL-2.0-or-later' \
    'dependency_2_election=Apache-2.0' \
    'dependency_2_election_notice=notices/mbedtls-Apache-2.0-election.txt' \
    'dependency_4_notice=notices/console-font-fixed8x16.txt'; do
    grep -Fq -- "$license_inventory_literal" \
        release/hfoc-license-inventory.txt || {
        echo "license inventory не содержит: $license_inventory_literal" >&2
        exit 1
    }
done
if grep -Eq '^dependency_2_(license|election)=(Apache-2\.0 OR GPL-2\.0-or-later|GPL-2\.0-or-later)$' \
        release/hfoc-license-inventory.txt; then
    echo "license inventory не фиксирует election как Apache-2.0" >&2
    exit 1
fi
grep -Fq 'HFOC elects Apache-2.0' notices/mbedtls-Apache-2.0-election.txt || {
    echo "mbedTLS election notice не фиксирует election" >&2
    exit 1
}
grep -Fq 'Apache-2.0 OR GPL-2.0-or-later' \
    notices/mbedtls-Apache-2.0-election.txt || {
    echo "mbedTLS election notice не называет upstream disjunction" >&2
    exit 1
}
# The election notice is part of the shipped payload, and the payload verifier
# forces it there.
# shellcheck disable=SC2016
grep -Fq 'notices/$path' scripts/install-license-payload.sh || {
    echo "install-license-payload.sh не устанавливает notices/" >&2
    exit 1
}
grep -Fq 'notices/mbedtls-Apache-2.0-election.txt' \
    scripts/verify-license-payload.sh || {
    echo "verify-license-payload.sh не требует election notice" >&2
    exit 1
}
grep -Fq 'verify-license-notices.sh' scripts/verify-license-payload.sh || {
    echo "verify-license-payload.sh не применяет notices контракт" >&2
    exit 1
}
# The release SBOM path records the election as the concluded license of the
# mbedTLS package, and keeps the upstream disjunction only as the declared one.
license_sbom_rows=$(bash scripts/emit-license-sbom-rows.sh) || {
    echo "emit-license-sbom-rows.sh не отработал" >&2
    exit 1
}
license_mbedtls_block=$(awk \
    '/^SPDXID: SPDXRef-Package-mbedtls$/ { inside = 1 } \
     inside && /^$/ { inside = 0 } \
     inside' <<<"$license_sbom_rows")
for license_sbom_literal in \
    'PackageLicenseConcluded: Apache-2.0' \
    'PackageLicenseDeclared: Apache-2.0 OR GPL-2.0-or-later' \
    'election recorded'; do
    grep -Fq -- "$license_sbom_literal" <<<"$license_mbedtls_block" || {
        echo "SBOM rows mbedTLS не содержат: $license_sbom_literal" >&2
        exit 1
    }
done
if grep -Fq 'PackageLicenseConcluded: Apache-2.0 OR GPL-2.0-or-later' \
        <<<"$license_sbom_rows"; then
    echo "SBOM rows публикуют disjunction как concluded license" >&2
    exit 1
fi
# The panel exposes the payload through the about/licenses page, inside the
# recorded per-page asset budget, and the panel refuses to invent a list.
for license_panel_literal in \
    APPLIANCE_MANAGEMENT_WEB_UI_LICENSES_PATH \
    APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_LICENSES; do
    grep -Fq "$license_panel_literal" \
        kernel/include/appliance_management_web_ui.h || {
        echo "management web UI не публикует $license_panel_literal" >&2
        exit 1
    }
done
for license_panel_symbol in \
    appliance_management_web_ui_render_licenses \
    appliance_management_panel_set_licenses \
    appliance_management_panel_publish_declared_licenses \
    appliance_management_panel_target_kind; do
    grep -Fq "$license_panel_symbol" \
        kernel/include/appliance_management_web_ui.h \
        kernel/include/appliance_management_panel.h || {
        echo "panel не публикует $license_panel_symbol" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LICENSES_PATH' \
    kernel/src/appliance_management_authorization.c || {
    echo "authorization matrix не знает about/licenses page" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_LICENSES_PATH' \
    kernel/include/appliance_management_authorization.h || {
    echo "authorization header не публикует about/licenses target" >&2
    exit 1
}
for license_page_target in \
    kernel/include/appliance_management_authorization.h \
    kernel/include/appliance_management_web_ui.h; do
    grep -Fq '"/panel/about/licenses"' "$license_page_target" || {
        echo "$license_page_target не публикует точный about/licenses target" >&2
        exit 1
    }
done
grep -Fq 'LICENSE_INVENTORY_UNPUBLISHED' \
    kernel/src/appliance_management_web_ui.c || {
    echo "licenses page не публикует UNAVAILABLE с причиной" >&2
    exit 1
}
grep -Fq 'LICENSE_PAYLOAD_INCOMPLETE' \
    kernel/src/appliance_management_web_ui.c || {
    echo "licenses page не публикует incomplete payload" >&2
    exit 1
}
grep -Fq 'licenses | 2736 | 2736 |' docs/hfoc-management-web-ui.md || {
    echo "management web UI document не записывает бюджет licenses page" >&2
    exit 1
}
if grep -nE '\b(malloc|calloc|free|kmalloc|kfree|pmm_alloc|pmm_free|serial_printf|printf|fprintf|snprintf|sprintf|serial_write|console_write|event_ring_|audit_|block_device_read|block_device_write)\b' \
        kernel/src/appliance_license_inventory.c; then
    echo "appliance_license_inventory.c использует запрещённое выделение, диагностику, аудит или I/O" >&2
    exit 1
fi
# LICENSE_PAYLOAD is ITEM-382's own acceptance stage and it may not be skipped.
grep -Fq 'stage_25_id=LICENSE_PAYLOAD' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "production acceptance matrix не содержит LICENSE_PAYLOAD stage" >&2
    exit 1
}
for license_stage_literal in \
    'stage_25_state=PROVEN' \
    'stage_25_kind=gate' \
    'stage_25_target=test-license-payload-host' \
    'stage_25_marker=License payload host test: OK'; do
    grep -Fq -- "$license_stage_literal" \
        release/hfoc-production-end-to-end-acceptance.txt || {
        echo "LICENSE_PAYLOAD stage не содержит: $license_stage_literal" >&2
        exit 1
    }
done
grep -Fq 'LICENSE_PAYLOAD' \
    scripts/verify-production-end-to-end-acceptance.sh || {
    echo "acceptance verifier не требует LICENSE_PAYLOAD stage" >&2
    exit 1
}
grep -Fq 'test-license-payload-host' \
    docs/hfoc-production-end-to-end-acceptance.md || {
    echo "acceptance document не называет LICENSE_PAYLOAD proof" >&2
    exit 1
}
grep -Fq 'License payload host test: OK' tests/test-license-payload-host.sh || {
    echo "license payload host proof не печатает marker" >&2
    exit 1
}
# The payload contract is verified end to end here: a freshly installed payload
# must satisfy every declared notice, and removing one notice or declaring a new
# dependency without its notice must fail closed with the dependency named.
license_payload_fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-license.XXXXXX")
trap 'rm -rf "$license_payload_fixture_dir"' EXIT
bash scripts/install-license-payload.sh \
    "$license_payload_fixture_dir/release" >/dev/null
bash scripts/verify-license-notices.sh \
    "$license_payload_fixture_dir/release/licenses" >/dev/null || {
    echo "license payload не содержит все declared notices" >&2
    exit 1
}
bash scripts/install-license-payload.sh \
    "$license_payload_fixture_dir/missing" >/dev/null
rm "$license_payload_fixture_dir/missing/licenses/third_party/lwip/COPYING"
if bash scripts/verify-license-notices.sh \
        "$license_payload_fixture_dir/missing/licenses" \
        >"$license_payload_fixture_dir/missing.log" 2>&1; then
    echo "license notices contract принял payload без notice" >&2
    exit 1
fi
grep -Fq 'lwip' "$license_payload_fixture_dir/missing.log" || {
    echo "license notices contract не назвал отсутствующую dependency" >&2
    exit 1
}
sed 's/^dependency_count=4$/dependency_count=5/' \
    release/hfoc-license-inventory.txt \
    >"$license_payload_fixture_dir/inventory.txt"
printf '%s\n' \
    'dependency_5_id=phantom' \
    'dependency_5_component=Phantom' \
    'dependency_5_version=1.0.0' \
    'dependency_5_license=MIT' \
    'dependency_5_notice=notices/phantom.txt' \
    >>"$license_payload_fixture_dir/inventory.txt"
if HFOC_LICENSE_INVENTORY="$license_payload_fixture_dir/inventory.txt" \
        bash scripts/verify-license-notices.sh \
        "$license_payload_fixture_dir/release/licenses" \
        >"$license_payload_fixture_dir/phantom.log" 2>&1; then
    echo "license notices contract принял dependency без notice" >&2
    exit 1
fi
grep -Fq 'phantom' "$license_payload_fixture_dir/phantom.log" || {
    echo "license notices contract не назвал dependency без notice" >&2
    exit 1
}
trap - EXIT
rm -rf "$license_payload_fixture_dir"
for license_surface_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'licenses page|license surface|about/licenses|licence surface' \
            "$license_surface_doc"; then
        echo "$license_surface_doc не документирует licenses surface" >&2
        exit 1
    fi
done
grep -Fq -- '- ITEM-382 (реализован)' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит прогресс по ITEM-382" >&2
    exit 1
}

# Integrated installer/dual-boot/management acceptance contracts (ITEM-346).
grep -Fq 'test-hfoc-integrated-installer-dual-boot-management-acceptance' Makefile || {
    echo "Makefile не содержит test-hfoc-integrated-installer-dual-boot-management-acceptance" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hfoc-integrated-installer-dual-boot-management-acceptance' Makefile || {
    echo "test не вызывает test-hfoc-integrated-installer-dual-boot-management-acceptance" >&2
    exit 1
}
test -x scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh || {
    echo "verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x scripts/test-hfoc-integrated-installer-dual-boot-management-acceptance.sh || {
    echo "test-hfoc-integrated-installer-dual-boot-management-acceptance.sh не исполняемый" >&2
    exit 1
}
test -x tests/test-hfoc-integrated-installer-dual-boot-management-acceptance-host.sh || {
    echo "test-hfoc-integrated-installer-dual-boot-management-acceptance-host.sh не исполняемый" >&2
    exit 1
}
bash -n scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh || {
    echo "Синтаксис verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-hfoc-integrated-installer-dual-boot-management-acceptance.sh || {
    echo "Синтаксис test-hfoc-integrated-installer-dual-boot-management-acceptance.sh некорректен" >&2
    exit 1
}
bash -n tests/test-hfoc-integrated-installer-dual-boot-management-acceptance-host.sh || {
    echo "Синтаксис test-hfoc-integrated-installer-dual-boot-management-acceptance-host.sh некорректен" >&2
    exit 1
}
test -f docs/hfoc-integrated-installer-dual-boot-management-acceptance.md || {
    echo "Отсутствует docs/hfoc-integrated-installer-dual-boot-management-acceptance.md" >&2
    exit 1
}
test -f docs/superpowers/specs/2026-09-06-item-346-integrated-acceptance-design.md || {
    echo "Отсутствует ITEM-346 design spec" >&2
    exit 1
}
test -f docs/superpowers/plans/2026-09-06-item-346-integrated-acceptance-implementation.md || {
    echo "Отсутствует ITEM-346 implementation plan" >&2
    exit 1
}
test -f release/hfoc-integrated-installer-dual-boot-management-acceptance.txt || {
    echo "Отсутствует integrated acceptance matrix" >&2
    exit 1
}
grep -Fq 'hfoc_integrated_installer_dual_boot_management_acceptance_version=1' \
    release/hfoc-integrated-installer-dual-boot-management-acceptance.txt || {
    echo "integrated acceptance matrix не содержит version 1" >&2
    exit 1
}
grep -Fq 'check_count=16' \
    release/hfoc-integrated-installer-dual-boot-management-acceptance.txt || {
    echo "integrated acceptance matrix не содержит 16 checks" >&2
    exit 1
}
grep -Fq 'QEMU and host proofs are not physical-hardware acceptance' \
    docs/hfoc-integrated-installer-dual-boot-management-acceptance.md || {
    echo "integrated acceptance doc не заявляет QEMU/host boundary" >&2
    exit 1
}
grep -Fq 'must remain NOT_TESTED' \
    docs/hfoc-integrated-installer-dual-boot-management-acceptance.md || {
    echo "integrated acceptance doc не заявляет physical NOT_TESTED boundary" >&2
    exit 1
}
bash scripts/verify-hfoc-integrated-installer-dual-boot-management-acceptance.sh \
    release/hfoc-integrated-installer-dual-boot-management-acceptance.txt >/dev/null
bash tests/test-hfoc-integrated-installer-dual-boot-management-acceptance-host.sh
for integrated_acceptance_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'integrated installer|integrated acceptance|installer/dual-boot/management acceptance' \
            "$integrated_acceptance_doc"; then
        echo "$integrated_acceptance_doc не документирует integrated acceptance" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **ITEM-346**' \
    docs/superpowers/plans/2026-09-06-hfoc-appliance-installer-and-management-ui.md || {
    echo "installer plan не помечает ITEM-346 завершённым" >&2
    exit 1
}

# NVMe production acceptance contracts (ITEM-348).
grep -Fq 'test-nvme-production-acceptance-host' Makefile || {
    echo "Makefile не содержит test-nvme-production-acceptance-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-nvme-production-acceptance-host' Makefile || {
    echo "test-host-suite не вызывает test-nvme-production-acceptance-host" >&2
    exit 1
}
grep -Fq 'test-nvme-production-acceptance:' Makefile || {
    echo "Makefile не содержит test-nvme-production-acceptance target" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Eq '\$\(MAKE\) test-nvme-production-acceptance[[:space:]]*$' Makefile || {
    echo "test не вызывает real NVMe production acceptance gate" >&2
    exit 1
}
grep -Fq 'scripts/test-hfoc-nvme-production-acceptance.sh' Makefile || {
    echo "Makefile не вызывает NVMe production acceptance runner" >&2
    exit 1
}
grep -Fq 'hfoc_nvme_production_acceptance_version=1' \
    release/hfoc-nvme-production-acceptance.txt || {
    echo "NVMe production acceptance matrix не содержит version 1" >&2
    exit 1
}
grep -Fq 'check_count=3' \
    release/hfoc-nvme-production-acceptance.txt || {
    echo "NVMe production acceptance matrix не содержит 3 checks" >&2
    exit 1
}
grep -Fq 'hardware_status=NOT_TESTED' \
    release/hfoc-nvme-production-acceptance.txt || {
    echo "NVMe production acceptance matrix не сохраняет NOT_TESTED" >&2
    exit 1
}
for nvme_acceptance_file in \
    scripts/verify-hfoc-nvme-production-acceptance.sh \
    scripts/test-hfoc-nvme-production-acceptance.sh \
    tests/test-hfoc-nvme-production-acceptance-host.sh; do
    test -x "$nvme_acceptance_file" || {
        echo "$nvme_acceptance_file не является исполняемым" >&2
        exit 1
    }
    bash -n "$nvme_acceptance_file" || {
        echo "Синтаксис $nvme_acceptance_file некорректен" >&2
        exit 1
    }
done
bash scripts/verify-hfoc-nvme-production-acceptance.sh \
    release/hfoc-nvme-production-acceptance.txt >/dev/null
bash scripts/test-hfoc-nvme-production-acceptance.sh >/dev/null
bash tests/test-hfoc-nvme-production-acceptance-host.sh

# Release documentation contracts (ITEM-358 closure).
grep -Fq 'test-hfoc-release-documentation' Makefile || {
    echo "Makefile не содержит test-hfoc-release-documentation" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-hfoc-release-documentation' Makefile || {
    echo "test не вызывает test-hfoc-release-documentation" >&2
    exit 1
}
test -x scripts/verify-hfoc-release-documentation.sh || {
    echo "scripts/verify-hfoc-release-documentation.sh не исполняемый" >&2
    exit 1
}
test -x scripts/test-hfoc-release-documentation.sh || {
    echo "scripts/test-hfoc-release-documentation.sh не исполняемый" >&2
    exit 1
}
test -x tests/test-hfoc-release-documentation-host.sh || {
    echo "tests/test-hfoc-release-documentation-host.sh не исполняемый" >&2
    exit 1
}
bash -n scripts/verify-hfoc-release-documentation.sh || {
    echo "Синтаксис verify-hfoc-release-documentation.sh некорректен" >&2
    exit 1
}
bash -n scripts/test-hfoc-release-documentation.sh || {
    echo "Синтаксис test-hfoc-release-documentation.sh некорректен" >&2
    exit 1
}
bash -n tests/test-hfoc-release-documentation-host.sh || {
    echo "Синтаксис test-hfoc-release-documentation-host.sh некорректен" >&2
    exit 1
}
for release_doc in docs/hfoc-installation-guide.md \
                   docs/hfoc-dual-boot-guide.md \
                   docs/hfoc-first-boot-guide.md \
                   docs/hfoc-administrator-guide.md \
                   docs/hfoc-recovery-guide.md \
                   docs/hfoc-supported-hardware-matrix.md \
                   docs/hfoc-known-limitations.md \
                   release/hfoc-hardware-acceptance-report.md; do
    test -f "$release_doc" || {
        echo "Отсутствует release documentation: $release_doc" >&2
        exit 1
    }
done
grep -Fq 'NOT TESTED' \
    docs/hfoc-supported-hardware-matrix.md release/hfoc-hardware-acceptance-report.md || {
    echo "hardware matrix/report не содержит NOT TESTED defaults" >&2
    exit 1
}
bash scripts/verify-hfoc-release-documentation.sh >/dev/null
bash tests/test-hfoc-release-documentation-host.sh
for release_documentation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'release documentation|Hardware Acceptance Report|Installation Guide' \
        "$release_documentation_doc" || {
        echo "$release_documentation_doc не документирует release documentation" >&2
        exit 1
    }
done
