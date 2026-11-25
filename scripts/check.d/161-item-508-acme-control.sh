#!/usr/bin/env bash
# ITEM-508 increment 34: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal Makefile and shell text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The durable ACME control: enable, disable and one pending issuance request.
#
# ITEM-508 point 10 makes enabling, disabling and starting an issuance
# two-phase operator mutations with an audit trail.  This increment owns the
# durable setting: one record next to the ITEM-503 certificate record and the
# ITEM-507 name table, a sixth mutation executor on the ITEM-367A plane, a
# panel control page of its own (the Dashboard is measured at its 2048-byte
# cap, so the forms do not fit there) and the service that reads the committed
# value.  The order path that consumes a pending request is the next increment;
# this slice pins that the request is published as requested and never reported
# as an order that started.
for item508_inc34_file in \
    kernel/include/appliance_acme_control.h \
    kernel/src/appliance_acme_control.c \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/include/appliance_production_acme_service.h \
    kernel/src/appliance_production_acme_service.c \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/src/kernel.c \
    tests/appliance-acme-control-host.c \
    tests/appliance-management-web-ui-host.c \
    tests/appliance-management-panel-host.c \
    Makefile \
    docs/hfoc-management-web-ui.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item508_inc34_file ]] || {
        echo "ITEM-508 inc34: отсутствует $item508_inc34_file" >&2
        exit 1
    }
done

# 1. The durable record: one block after the certificate record (12) and the
#    name table (32), with a magic, a version, a checksum and the two control
#    bits; disabling clears a pending request and a request needs ACME enabled.
for item508_inc34_record_pin in \
    'APPLIANCE_ACME_CONTROL_MAGIC UINT32_C(0x434D4341)' \
    'APPLIANCE_ACME_CONTROL_RECORD_VERSION 1U' \
    'APPLIANCE_ACME_CONTROL_BLOCK_OFFSET' \
    'APPLIANCE_ACME_CONTROL_RECORD_BLOCKS 1U' \
    'APPLIANCE_ACME_CONTROL_FLAG_ENABLED' \
    'APPLIANCE_ACME_CONTROL_FLAG_ORDER_REQUESTED' \
    'APPLIANCE_ACME_CONTROL_NOT_ENABLED' \
    'APPLIANCE_ACME_CONTROL_ALREADY_REQUESTED'; do
    grep -Fq -- "$item508_inc34_record_pin" \
        kernel/include/appliance_acme_control.h || {
        echo "ITEM-508 inc34: заголовок записи не содержит «$item508_inc34_record_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS +' \
    kernel/include/appliance_acme_control.h || {
    echo "ITEM-508 inc34: запись не отсчитана от записи сертификата" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CERTIFICATE_NAME_STORE_RECORD_BLOCKS' \
    kernel/include/appliance_acme_control.h || {
    echo "ITEM-508 inc34: запись не отсчитана от таблицы имён" >&2
    exit 1
}
for item508_inc34_store_pin in \
    'control->order_requested = false;' \
    'appliance_acme_control_store_save' \
    'appliance_state_block_device_read(' \
    'memcmp(verify, record, sizeof(record))'; do
    grep -Fq -- "$item508_inc34_store_pin" \
        kernel/src/appliance_acme_control.c || {
        echo "ITEM-508 inc34: модуль записи не содержит «$item508_inc34_store_pin»" \
            >&2
        exit 1
    }
done

# 2. The host proof drives the record against a memory device and pins the
#    placement, the off default, the single request, the clear-on-disable rule
#    and every corrupt-record refusal.
for item508_inc34_proof_pin in \
    'static void plant_record(' \
    'a blank control record leaves ACME disabled' \
    'an order request while ACME is disabled is refused by name' \
    'a second request while one is pending is refused, not queued' \
    'the pending request survives the reboot' \
    'disabling clears the pending order request' \
    'a record with a bad checksum is refused as a checksum fault' \
    'a refused record yields the off state, never half a ' \
    'a record the device did not keep fails the save'; do
    grep -Fq -- "$item508_inc34_proof_pin" \
        tests/appliance-acme-control-host.c || {
        echo "ITEM-508 inc34: проба записи не содержит «$item508_inc34_proof_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_ACME_CONTROL_BLOCK_OFFSET == 44U' \
    tests/appliance-acme-control-host.c || {
    echo "ITEM-508 inc34: проба не пинит смещение записи 44" >&2
    exit 1
}
grep -Fq 'test-appliance-acme-control-host' Makefile || {
    echo "ITEM-508 inc34: Makefile не объявляет цель пробы записи" >&2
    exit 1
}
grep -Fq '	$(MAKE) test-appliance-acme-control-host' Makefile || {
    echo "ITEM-508 inc34: хост-набор не исполняет пробу записи" >&2
    exit 1
}

# 3. The authorization surface: a seventeenth command id, an eleventh mutation
#    and the two exact targets.  The matrix gains one entry and its count moves.
for item508_inc34_auth_pin in \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACME_MUTATION 17U' \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_ACME_CONTROL = 11,' \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_ACME_PATH "/api/v1/system/acme"' \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH "/panel/acme"'; do
    grep -Fq -- "$item508_inc34_auth_pin" \
        kernel/include/appliance_management_authorization.h || {
        echo "ITEM-508 inc34: политика не содержит «$item508_inc34_auth_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'struct authorization_entry entries[17];' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-508 inc34: матрица не выросла до шестнадцати записей" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_ACME_MUTATION,' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-508 inc34: матрица не несёт команду ACME" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_ACME_PATH,' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-508 inc34: панельная цель не признана панельной" >&2
    exit 1
}
grep -Fq 'return "ACME_CONTROL";' \
    kernel/src/appliance_management_authorization.c || {
    echo "ITEM-508 inc34: мутация не названа в записи аудита" >&2
    exit 1
}

# 4. The mutation plane: a bound executor, the stage and the commit that proves
#    the durable write.  An unbound executor stays an unsupported target.
for item508_inc34_plane_pin in \
    'struct appliance_production_mutation_plane_acme_executor' \
    'appliance_production_mutation_plane_bind_acme' \
    'appliance_production_mutation_plane_acme_ready' \
    'plane->acme.commit(plane->acme.context)' \
    'plane->acme.stage(' \
    'plane->acme.rollback(plane->acme.context)'; do
    grep -Fq -- "$item508_inc34_plane_pin" \
        kernel/include/appliance_production_mutation_plane.h \
        kernel/src/appliance_production_mutation_plane.c || {
        echo "ITEM-508 inc34: плоскость мутаций не содержит «$item508_inc34_plane_pin»" \
            >&2
        exit 1
    }
done

# 5. The serving composition: the executor, the two HTTP targets, the record
#    and the getter the ordinary boot reads.  The record names the committed
#    values; nothing claims an order started.
for item508_inc34_serving_pin in \
    'appliance_production_serving_acme_stage' \
    'appliance_production_serving_acme_commit' \
    'appliance_production_serving_acme_rollback' \
    'appliance_production_serving_handle_acme' \
    'appliance_production_serving_load_acme_control' \
    'appliance_production_serving_publish_acme_control' \
    'appliance_production_serving_acme_control' \
    '[PRODUCTION-ACME-CONTROL] source=PANEL action=' \
    'applies=service'; do
    grep -Fq -- "$item508_inc34_serving_pin" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-508 inc34: serving не содержит «$item508_inc34_serving_pin»" \
            >&2
        exit 1
    }
done
for item508_inc34_target_pin in \
    'HTTP_POLICY_ACME_TARGET' \
    'HTTP_POLICY_PANEL_ACME_TARGET'; do
    grep -Fq -- "$item508_inc34_target_pin" kernel/src/http_parser.c || {
        echo "ITEM-508 inc34: таблица форм не несёт «$item508_inc34_target_pin»" \
            >&2
        exit 1
    }
done

# 6. The service follows the control: a disabled service does nothing and is
#    named, and the request is held as requested, not executed here.
for item508_inc34_service_pin in \
    'APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_DISABLED = 5,' \
    'appliance_production_acme_service_set_control' \
    'service->control_published && !service->enabled' \
    'return "DISABLED";'; do
    grep -Fq -- "$item508_inc34_service_pin" \
        kernel/include/appliance_production_acme_service.h \
        kernel/src/appliance_production_acme_service.c || {
        echo "ITEM-508 inc34: служба не содержит «$item508_inc34_service_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'appliance_production_acme_service_set_control(' kernel/src/kernel.c || {
    echo "ITEM-508 inc34: обычная загрузка не передаёт службе управление" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_acme_control()' kernel/src/kernel.c || {
    echo "ITEM-508 inc34: обычная загрузка не читает запись управления" >&2
    exit 1
}
grep -Fq 'log_production_acme_control();' kernel/src/kernel.c || {
    echo "ITEM-508 inc34: запись мутации не публикуется в журнал" >&2
    exit 1
}

# 7. The panel page: a page of its own with its own budget, its own target kind
#    and the four forms; the Dashboard links to it.
for item508_inc34_panel_pin in \
    'APPLIANCE_MANAGEMENT_PANEL_TARGET_ACME = 5,' \
    'appliance_management_panel_set_acme_control' \
    'appliance_management_web_ui_render_acme_control' \
    'panel->acme_page'; do
    grep -Fq -- "$item508_inc34_panel_pin" \
        kernel/include/appliance_management_panel.h \
        kernel/src/appliance_management_panel.c || {
        echo "ITEM-508 inc34: панель не содержит «$item508_inc34_panel_pin»" \
            >&2
        exit 1
    }
done
for item508_inc34_web_pin in \
    '#define APPLIANCE_MANAGEMENT_WEB_UI_ACME_PATH "/panel/acme"' \
    'APPLIANCE_MANAGEMENT_WEB_UI_PAGE_ACME = 15,' \
    'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_ACME 2797U'; do
    grep -Fq -- "$item508_inc34_web_pin" \
        kernel/include/appliance_management_web_ui.h || {
        echo "ITEM-508 inc34: панельный вид не содержит «$item508_inc34_web_pin»" \
            >&2
        exit 1
    }
done
grep -Fq 'static void test_acme_control_page(void)' \
    tests/appliance-management-web-ui-host.c || {
    echo "ITEM-508 inc34: панельная проба не проверяет страницу управления" >&2
    exit 1
}
grep -Fq '| acme | 2797 | 2797 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-508 inc34: документация не несёт строку бюджета страницы" >&2
    exit 1
}

# 8. The documents name the increment.
for item508_inc34_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    grep -Fq 'ITEM-508 increment 34' "$item508_inc34_doc" || {
        echo "ITEM-508 inc34: $item508_inc34_doc не называет инкремент" >&2
        exit 1
    }
done
