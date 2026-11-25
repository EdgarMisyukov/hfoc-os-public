#!/usr/bin/env bash
# ITEM-516: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The owner's decision of 2026-10-10 (variant "b"): the operator may save the
# decision "the panel is available after boot without a local login".  The item
# has four contracted halves and this slice holds each of them:
#
#   * the durable setting is the third optional key of the system record,
#     `v1:security.panel.after-boot=open`; `closed` is the default and is never
#     written, so a record without the key is byte-identical to the previous
#     format;
#   * the boot reads it and names what it did: `gate=PUBLISHED source=POLICY`
#     against `gate=CLOSED reason=AWAITING_LOCAL_SECRET`;
#   * enabling is local-only and requires a proven local secret, and the
#     operator must read the full warning (physical presence, password and
#     lockout, audit finding R01's wildcard listener);
#   * disabling is available from the panel through the ordinary two-phase
#     mutation, which can only ever submit `closed` and refuses `open` by name.
for item516_file in \
    kernel/include/appliance_config_validation.h \
    kernel/src/appliance_config_validation.c \
    kernel/include/appliance_persistent_config.h \
    kernel/src/appliance_persistent_config.c \
    kernel/include/appliance_panel_gate.h \
    kernel/src/appliance_panel_gate.c \
    kernel/include/console_gate_command.h \
    kernel/src/console.c \
    kernel/include/appliance_production_admin_enrollment.h \
    kernel/src/appliance_production_admin_enrollment.c \
    kernel/src/kernel.c \
    kernel/include/appliance_management_authorization.h \
    kernel/include/authorization.h \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/src/appliance_production_serving.c \
    kernel/include/appliance_management_web_ui.h \
    kernel/src/appliance_management_web_ui.c \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c \
    kernel/src/appliance_language.c \
    tests/appliance-panel-gate-host.c \
    tests/appliance-persistent-storage-namespace-host.c \
    tests/appliance-production-admin-enrollment-host.c \
    tests/console-host.c \
    tests/appliance-production-mutation-plane-host.c \
    tests/appliance-panel-forms-host.c \
    tests/appliance-management-authorization-host.c \
    tests/appliance-management-panel-host.c \
    tests/appliance-management-web-ui-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/hfoc-production-admin-enrollment.md \
    docs/hfoc-management-web-ui.md \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item516_file ]] || {
        echo "ITEM-516: отсутствует $item516_file" >&2
        exit 1
    }
done

# 1. The durable record: the exact key, the two values, the named refusal of
#    anything else, and the default that is never written.
grep -Fq '"v1:security.panel.after-boot="' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-516: ключ настройки не объявлен" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT = 9' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-516: отказ по значению настройки не назван" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_OPEN "open"' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-516: значение open не объявлено" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_CLOSED "closed"' \
    kernel/include/appliance_config_validation.h || {
    echo "ITEM-516: значение closed не объявлено" >&2
    exit 1
}
grep -Fq 'appliance_config_validation_validate_panel_after_boot(' \
    kernel/src/appliance_config_validation.c || {
    echo "ITEM-516: значение настройки не проверяется по имени" >&2
    exit 1
}
grep -Fq 'appliance_persistent_config_set_panel_after_boot(' \
    kernel/include/appliance_persistent_config.h || {
    echo "ITEM-516: установка настройки не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT = 11' \
    kernel/include/appliance_persistent_config.h || {
    echo "ITEM-516: отказ хранилища по настройке не назван" >&2
    exit 1
}
# The default value writes no key: the builder appends the line only for open.
grep -Fq 'if (panel_after_boot_open) {' \
    kernel/src/appliance_persistent_config.c || {
    echo "ITEM-516: closed снова пишется в запись" >&2
    exit 1
}
grep -Fq 'config->values.panel_after_boot_open, record' \
    kernel/src/appliance_persistent_config.c || {
    echo "ITEM-516: смена hostname теряет настройку" >&2
    exit 1
}
grep -Fq 'config->values.language, open, record' \
    kernel/src/appliance_persistent_config.c || {
    echo "ITEM-516: смена настройки теряет язык" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_CONFIG_INTEGRITY_RECORD_COUNT 11U' \
    kernel/include/appliance_config_integrity.h || {
    echo "ITEM-516: каталог целостности не пересчитан" >&2
    exit 1
}
grep -Fq 'APPLIANCE_CONFIG_INTEGRITY_KIND_SECURITY_PANEL_AFTER_BOOT = 11' \
    kernel/include/appliance_config_integrity.h || {
    echo "ITEM-516: вид записи настройки не объявлен" >&2
    exit 1
}
grep -Fq '"validated management gate after-boot policy"' \
    kernel/src/appliance_config_integrity.c || {
    echo "ITEM-516: запись настройки не описана в каталоге" >&2
    exit 1
}
# The same key line belongs to the system-configuration store, so the storage
# namespace inventory learns its prefix too.
for item516_ns in \
    'APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX 3U' \
    '"v1:security."'; do
    grep -Fq -- "$item516_ns" \
        kernel/include/appliance_persistent_storage_namespace.h \
        kernel/src/appliance_persistent_storage_namespace.c || {
        echo "ITEM-516: инвентарь хранилища не знает ключ: $item516_ns" >&2
        exit 1
    }
done
grep -Fq 'the management-gate key classifies to system configuration' \
    tests/appliance-persistent-storage-namespace-host.c || {
    echo "ITEM-516: проба инвентаря хранилища не написана" >&2
    exit 1
}

# 2. The boot publication: both exact records, and the policy publication that
#    needs no local login.
grep -Fq 'gate=PUBLISHED source=POLICY' \
    kernel/src/appliance_panel_gate.c || {
    echo "ITEM-516: запись публикации политики не написана" >&2
    exit 1
}
grep -Fq 'gate=CLOSED reason=AWAITING_LOCAL_SECRET' \
    kernel/src/appliance_panel_gate.c || {
    echo "ITEM-516: запись закрытого гейта не написана" >&2
    exit 1
}
grep -Fq 'appliance_production_admin_enrollment_publish_policy(' \
    kernel/src/appliance_production_admin_enrollment.c \
    kernel/src/kernel.c || {
    echo "ITEM-516: загрузка не публикует сохранённый верификатор" >&2
    exit 1
}
grep -Fq 'publish_production_panel_gate();' kernel/src/kernel.c || {
    echo "ITEM-516: решение гейта не публикуется при загрузке" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PANEL_GATE_EVENT_BOOT_PUBLISHED' kernel/src/kernel.c || {
    echo "ITEM-516: загрузка не различает политику и умолчание" >&2
    exit 1
}
# The console's one durable surface is wired in the composition: without the
# binding the command can never write on a real machine, and the boot must
# publish both the binding and the setting it read.
for item516_binding in \
    'production_panel_gate_read' \
    'production_panel_gate_write' \
    'appliance_persistent_config_set_panel_after_boot(config, open)' \
    'console_bind_gate(&production_panel_gate_binding);' \
    'console_publish_gate(open, durable);'; do
    grep -Fq -- "$item516_binding" kernel/src/kernel.c || {
        echo "ITEM-516: консоль не привязана к записи гейта: $item516_binding" >&2
        exit 1
    }
done

# 3. Enabling: the console grammar, the local proof and the warning.
grep -Fq '#define CONSOLE_GATE_COMMAND_OPEN_CONFIRM_WORD "gate open confirm"' \
    kernel/include/console_gate_command.h || {
    echo "ITEM-516: подтверждение включения не объявлено" >&2
    exit 1
}
grep -Fq 'console_gate_command_classify(input_line, input_length,' \
    kernel/src/console.c || {
    echo "ITEM-516: словарь гейта не разбирается перед цепочкой команд" >&2
    exit 1
}
grep -Fq 'console_gate_record_local_proof();' kernel/src/console.c || {
    echo "ITEM-516: локальное доказательство секрета не ставится" >&2
    exit 1
}
grep -Fq 'if (!console_gate_proven) {' kernel/src/console.c || {
    echo "ITEM-516: включение не требует доказанного секрета" >&2
    exit 1
}
for item516_warning in \
    'console.gate.warning_no_presence' \
    'console.gate.warning_password_only' \
    'console.gate.warning_lockout' \
    'console.gate.warning_wildcard' \
    'console.gate.warning_internet'; do
    grep -Fq -- "$item516_warning" kernel/src/console.c \
        kernel/src/appliance_language.c || {
        echo "ITEM-516: предупреждение не называет: $item516_warning" >&2
        exit 1
    }
done
grep -Fq 'Audit finding R01' kernel/src/appliance_language.c || {
    echo "ITEM-516: предупреждение не называет находку R01" >&2
    exit 1
}
grep -Fq 'wildcard address' kernel/src/appliance_language.c || {
    echo "ITEM-516: предупреждение не называет wildcard-адрес" >&2
    exit 1
}
grep -Fq 'open to the whole network' kernel/src/appliance_language.c || {
    echo "ITEM-516: предупреждение не называет открытую сеть" >&2
    exit 1
}

# 4. Disabling: the panel target, the authorization matrix, the two-phase
#    executor and the named refusal - and no path from the panel to `open`.
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH "/panel/gate"' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-516: цель страницы гейта не объявлена" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_GATE_MUTATION 18U' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-516: команда матрицы для гейта не объявлена" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_PANEL_GATE = 12,' \
    kernel/include/appliance_management_authorization.h || {
    echo "ITEM-516: мутация гейта не объявлена" >&2
    exit 1
}
grep -Fq '#define AUTHORIZATION_MATRIX_CAPACITY_MAX 17U' \
    kernel/include/authorization.h || {
    echo "ITEM-516: ёмкость матрицы не пересчитана" >&2
    exit 1
}
for item516_target in \
    'HTTP_POLICY_GATE_TARGET "/api/v1/system/gate"' \
    'HTTP_POLICY_PANEL_GATE_TARGET "/panel/gate"'; do
    grep -Fq -- "$item516_target" kernel/include/http_parser.h || {
        echo "ITEM-516: цель формы не объявлена: $item516_target" >&2
        exit 1
    }
done
grep -Fq '{HTTP_POLICY_GATE_TARGET, HTTP_POLICY_GATE_TARGET_LENGTH},' \
    kernel/src/http_parser.c || {
    echo "ITEM-516: скриптовая цель не в политике форм" >&2
    exit 1
}
grep -Fq 'appliance_production_mutation_plane_bind_panel_gate(' \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-516: исполнитель гейта не связывается" >&2
    exit 1
}
grep -Fq 'appliance_persistent_config_set_panel_after_boot(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-516: коммит не пишет безопасное направление" >&2
    exit 1
}
grep -Fq 'strcmp(action_text, "close") == 0' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-516: стадия не принимает только закрытие" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_SERVING_GATE_LOCAL_SECRET_REQUIRED' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-516: отказ панели не назван" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PANEL_GATE_LOCAL_SECRET_REQUIRED' \
    kernel/include/appliance_panel_gate.h || {
    echo "ITEM-516: стабильное имя отказа не объявлено" >&2
    exit 1
}
grep -Fq 'name=\"action\" value=\"close\"' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-516: страница рисует не только закрытие" >&2
    exit 1
}
if grep -Fq 'value=\"open\"' kernel/src/appliance_management_web_ui.c; then
    echo "ITEM-516: страница панели предлагает открыть гейт" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_MANAGEMENT_PANEL_TARGET_GATE' \
    kernel/include/appliance_management_panel.h \
    kernel/src/appliance_management_panel.c || {
    echo "ITEM-516: страница гейта не классифицируется" >&2
    exit 1
}
grep -Fq 'appliance_management_web_ui_render_gate(' \
    kernel/src/appliance_management_web_ui.c || {
    echo "ITEM-516: страница гейта не рендерится" >&2
    exit 1
}
for item516_panel in \
    'panel.gate.warning_no_presence' \
    'panel.gate.warning_lockout' \
    'panel.gate.warning_wildcard' \
    'panel.gate.local_only' \
    'panel.gate.close'; do
    grep -Fq -- "$item516_panel" kernel/src/appliance_management_web_ui.c \
        kernel/src/appliance_language.c || {
        echo "ITEM-516: страница не называет: $item516_panel" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_MANAGEMENT_AUTHORIZATION_PANEL_GATE_PATH' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-516: 303 панели не возвращает на страницу гейта" >&2
    exit 1
}

# 5. The proofs exist, are wired into the build and the host suite, and the
#    drawn body rule covers the new page.
for item516_host in \
    'static void test_panel_gate_executor(void)' \
    'gate executor: opening from the panel is refused' \
    'gate executor: the refused open issues no token and writes '; do
    grep -Fq -- "$item516_host" \
        tests/appliance-production-mutation-plane-host.c || {
        echo "ITEM-516: проба плоскости не несёт: $item516_host" >&2
        exit 1
    }
done
for item516_console in \
    'the refusal names the local secret requirement' \
    'the warning names the loss of physical presence' \
    'gate close writes the default closed without any proof'; do
    grep -Fq -- "$item516_console" tests/console-host.c || {
        echo "ITEM-516: консольная проба не несёт: $item516_console" >&2
        exit 1
    }
done
for item516_forms in \
    'static void test_gate_drawn_bodies(void)' \
    'ITEM-516: the gate action form can only submit close' \
    'ITEM-516: the gate page draws no control that opens the gate'; do
    grep -Fq -- "$item516_forms" tests/appliance-panel-forms-host.c || {
        echo "ITEM-516: проба форм не несёт: $item516_forms" >&2
        exit 1
    }
done
for item516_record in \
    'the policy publication record is exact' \
    'the awaiting-local-secret record is exact'; do
    grep -Fq -- "$item516_record" tests/appliance-panel-gate-host.c || {
        echo "ITEM-516: проба записей не несёт: $item516_record" >&2
        exit 1
    }
done
grep -Fq 'policy publication opens the gate without a local login' \
    tests/appliance-production-admin-enrollment-host.c || {
    echo "ITEM-516: проба публикации политики не написана" >&2
    exit 1
}
grep -Fq 'test-appliance-panel-gate-host' Makefile || {
    echo "ITEM-516: проба записей не подключена к сборке" >&2
    exit 1
}
grep -Fq '$(MAKE) test-appliance-panel-gate-host' Makefile || {
    echo "ITEM-516: проба записей не вызвана в наборе" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_GATE 3648U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-516: бюджет страницы гейта не пере-измерен" >&2
    exit 1
}
grep -Fq '| gate | 3648 | 3648 |' docs/hfoc-management-web-ui.md || {
    echo "ITEM-516: документ не называет замер страницы гейта" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_WEB_UI_BUDGET_SECURITY 3209U' \
    kernel/include/appliance_management_web_ui.h || {
    echo "ITEM-516: бюджет страницы Security не пере-измерен" >&2
    exit 1
}

# 6. The QEMU proof: the open-record boot serves without a local login, the
#    enable gesture is the console's own, and the default boot still answers
#    AUTH_UNAVAILABLE before a local login.
for item516_qemu in \
    'POLICY_SERIAL=' \
    'POLICY_MONITOR_SOCKET=' \
    'gate=PUBLISHED source=POLICY' \
    'gate=CLOSED reason=AWAITING_LOCAL_SECRET' \
    'gate open' \
    'gate open confirm' \
    'server_hostname=None if expect_open' \
    'TLS-PROOF policy boot: the panel answers after boot with no local login' \
    'TLS-PROOF policy boot: the default boot still answers AUTH_UNAVAILABLE'; do
    grep -Fq -- "$item516_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-516: QEMU-проба не несёт: $item516_qemu" >&2
        exit 1
    }
done
# The enable gesture is a continuation of the credential durability boot's own
# proven login: one process, one monitor connection, no second client that
# could start before the console accepts input, and no password typed twice.
for item516_enable in \
    'ACCOUNT_DURABILITY_TOKEN=$(python3 "$TEST_DIR/admin-enroll.py" login-gate' \
    'if mode == "login-gate":' \
    'sys.stderr.write("gate enable: the console wrote the policy' \
    'TLS-PROOF gate enable: the local console wrote the policy after a proven local secret' \
    'gate_closed_line=' \
    'gate_pending_line=' \
    'gate_enabled_line=' \
    'did not publish CLOSED, PENDING and ENABLED in order' \
    'ADMIN SESSION: Authorization: Bearer ' \
    'for attempt in range(3):' \
    'gate enable: attempt %d did not finish' \
    'gate enable: no gate=ENABLED record after 3 full ' \
    'def gate_messages():' \
    'dump_gate_diagnostics' \
    'MANAGEMENT GATE / REFUSED lines'; do
    grep -Fq -- "$item516_enable" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-516: пробу включения не несёт: $item516_enable" >&2
        exit 1
    }
done
# The confirmation consumes the armed request even on failure, so the retry
# must be the whole gesture: a fresh `gate open` re-arms it, and success is the
# ENABLED record alone.
grep -Fq 'type_until("confirmation_required", "gate open", 60.0)' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-516: повтор не пере-взводит подтверждение" >&2
    exit 1
}
grep -Fq 'type_until("gate=ENABLED source=LOCAL_SECRET",' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-516: подтверждение не ждёт запись ENABLED" >&2
    exit 1
}
# The token contract: the command substitution still captures exactly one
# stdout line, and the gate progress is stderr-only.
grep -Fq 'print(token)' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-516: контракт stdout токена потерян" >&2
    exit 1
}
if grep -Fq 'print("gate-open")' scripts/test-qemu-production-service-https.sh; then
    echo "ITEM-516: режим включения печатает в stdout" >&2
    exit 1
fi
# Exactly one site may enable the policy, and no boot of its own may come back:
# the prompt-waiting second client is the shape CI proved unreliable.
if [[ $(grep -c 'admin-enroll.py" login-gate' \
        scripts/test-qemu-production-service-https.sh) -ne 1 ]]; then
    echo "ITEM-516: включение гейта выполняется не на одной площадке" >&2
    exit 1
fi
for item516_gone in \
    'SERVING_GATE_ENABLE_PORT' \
    'GATE_ENABLE_SERIAL' \
    'gate-after-login' \
    'wait_for("hfoc>", 120.0)'; do
    if grep -Fq -- "$item516_gone" \
            scripts/test-qemu-production-service-https.sh; then
        echo "ITEM-516: отдельная загрузка включения вернулась: $item516_gone" >&2
        exit 1
    fi
done

# 7. The item is recorded where the next window reads it.
for item516_doc in docs/ROADMAP.md docs/ARCHITECTURE.md \
    docs/hfoc-management-web-ui.md docs/hfoc-production-admin-enrollment.md; do
    grep -Fq 'ITEM-516' "$item516_doc" || {
        echo "ITEM-516: $item516_doc не называет пункт" >&2
        exit 1
    }
done
# ITEM-515 moved the queue's `## Current item` heading on to its own item, and
# ITEM-517 (ITEM-516's successor) moved it on again, so this pin reads
# ITEM-516's own historical section; the heading shape itself is held by
# `scripts/check.d/52-queue-current-item.sh`.
grep -A 3 '^## Item ITEM-516$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-516' || {
    echo "ITEM-516: раздел очереди не называет ITEM-516" >&2
    exit 1
}
