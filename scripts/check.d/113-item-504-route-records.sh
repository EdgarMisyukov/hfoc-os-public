#!/usr/bin/env bash
# ITEM-504 increment 5: contracted slice of the HFOC OS check suite.  Sourced in
# pinned order by scripts/check.sh; lint it on its own with shellcheck.

# Every routing change names itself.  The mutation's terminal state leaves one
# bounded record body in the routing composition, and the ordinary boot drains
# it as `[PRODUCTION-ROUTE]`:
#
#   * a commit records the applied route (`result=APPLIED`), a cancellation
#     records the dropped candidate (`result=CANCELLED`), and a refusal records
#     the requested name together with the model's own reason
#     (`result=REFUSED reason=DUPLICATE_NAME`, `... BACKEND_NOT_HOST`, ...);
#   * a stage that only holds a candidate records nothing: the record appears
#     when the state is terminal, so it says what was applied, not what was
#     offered;
#   * a name the model did not normalize is echoed with every byte outside the
#     visible range replaced by `?`, so a hostile value cannot start a second
#     log line;
#   * `take_record` hands the body over exactly once, and a buffer that cannot
#     hold the whole body takes nothing and loses nothing;
#   * the serving path only moves the body out (`take_route_record`), the boot
#     loop writes the marker and mirrors the line into the durable log;
#   * both outcomes are proven end to end: the QEMU routing boot commits a
#     route and refuses a duplicate, and both records must be in the serial log.

for item504e_file in \
    kernel/include/appliance_route_config.h \
    kernel/src/appliance_route_config.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    tests/appliance-route-config-host.c \
    scripts/test-qemu-production-service-https.sh; do
    test -f "$item504e_file" || {
        echo "ITEM-504E: отсутствует файл: $item504e_file" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the record API --
for item504e_api in \
    APPLIANCE_ROUTE_CONFIG_RECORD_MAX \
    APPLIANCE_ROUTE_CONFIG_RECORD_NONE \
    APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED \
    APPLIANCE_ROUTE_CONFIG_RECORD_REFUSED \
    APPLIANCE_ROUTE_CONFIG_RECORD_CANCELLED \
    appliance_route_config_take_record \
    appliance_route_config_record_result \
    appliance_route_config_record_count \
    appliance_route_config_record_result_string; do
    grep -Fq "$item504e_api" kernel/include/appliance_route_config.h || {
        echo "ITEM-504E: композиция маршрутов не публикует: $item504e_api" >&2
        exit 1
    }
done
grep -Fq 'record_pending' kernel/include/appliance_route_config.h || {
    echo "ITEM-504E: у композиции нет слота записи" >&2
    exit 1
}
grep -Fq 'record[APPLIANCE_ROUTE_CONFIG_RECORD_MAX + 1U]' \
    kernel/include/appliance_route_config.h || {
    echo "ITEM-504E: слот записи не ограничен" >&2
    exit 1
}

# ---------------------------------------------------------- the record build ---
for item504e_word in \
    '" result="' \
    'result=REFUSED reason=' \
    'result_word' \
    '"APPLIED"' \
    '"CANCELLED"'; do
    grep -Fq -- "$item504e_word" kernel/src/appliance_route_config.c || {
        echo "ITEM-504E: запись не называет: $item504e_word" >&2
        exit 1
    }
done

# The record body is the route table's own render, never a paraphrase.
grep -Fq 'appliance_route_render(&config->operation' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: запись маршрута не рендерит модель" >&2
    exit 1
}

# A refused stage records the model's own reason, not a generic failure.
grep -Fq 'appliance_route_status_string(route_status)' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: отказ не называет причину модели" >&2
    exit 1
}
grep -Fq 'appliance_route_config_status_string(config->last_status)' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: отказ до модели не называет свой статус" >&2
    exit 1
}

# A hostile, un-normalized name cannot start a second record line.
grep -Fq 'byte >= 0x21U && byte <= 0x7EU' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: имя отказа не экранируется" >&2
    exit 1
}
grep -Fq ": '?';" kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: управляющий байт имени не становится видимым" >&2
    exit 1
}

# A short buffer takes nothing and loses nothing.
grep -Fq 'config->record_length + 1U > capacity' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: короткий буфер не отвергается" >&2
    exit 1
}
grep -Fq 'config->record_pending = false;' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: запись не отдаётся ровно один раз" >&2
    exit 1
}

# A stage that only holds a candidate leaves no record: the record calls live
# in commit, rollback and the refusal paths, never on the success path.
if sed -n \
        '/^enum appliance_route_config_status appliance_route_config_stage/,/^}/p' \
        kernel/src/appliance_route_config.c | \
        grep -Fq 'route_config_record_operation'; then
    echo "ITEM-504E: стадия публикует запись до терминального состояния" >&2
    exit 1
fi
grep -Fq 'route_config_record_operation(config, APPLIANCE_ROUTE_CONFIG_RECORD_APPLIED' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: коммит не называет применённый маршрут" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_CONFIG_RECORD_CANCELLED, "CANCELLED"' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: отмена не называет брошенного кандидата" >&2
    exit 1
}
grep -Fq 'config->operation_set = false;' \
    kernel/src/appliance_route_config.c || {
    echo "ITEM-504E: отказ не забывает несостоявшуюся операцию" >&2
    exit 1
}

# ------------------------------------------------------- the serving hand-over --
grep -Fq 'appliance_production_serving_take_route_record' \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504E: путь обслуживания не отдаёт запись" >&2
    exit 1
}
grep -Fq 'appliance_route_config_take_record(&serving->route_config' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504E: путь обслуживания не читает запись композиции" >&2
    exit 1
}

# ------------------------------------------------------------- the boot record --
grep -Fq 'static void log_production_route(void)' kernel/src/kernel.c || {
    echo "ITEM-504E: цикл загрузки не публикует запись маршрутов" >&2
    exit 1
}
grep -Fq '[PRODUCTION-ROUTE] ' kernel/src/kernel.c || {
    echo "ITEM-504E: запись не называет свой маркер" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_take_route_record(' kernel/src/kernel.c || {
    echo "ITEM-504E: цикл загрузки не дренирует запись" >&2
    exit 1
}
grep -Fq 'console_record_text(record);' kernel/src/kernel.c || {
    echo "ITEM-504E: запись не попадает в durable-журнал" >&2
    exit 1
}
grep -Fq 'log_production_route();' kernel/src/kernel.c || {
    echo "ITEM-504E: запись не вызывается из цикла" >&2
    exit 1
}

# ------------------------------------------------------------- the host proof --
for item504e_proof in \
    'an initialized composition left no record' \
    'a composition with no terminal state hands over no record' \
    'a held candidate leaves no record until it is terminal' \
    'the commit leaves a record' \
    'the record names the applied route and its result' \
    'the record is handed over exactly once' \
    'a cancel leaves a record' \
    'the record names the dropped route and its result' \
    'the refusal leaves a record' \
    "the refusal names the requested name and the model's reason" \
    'a rollback after a refusal is refused' \
    'the refusal names the backend reason' \
    'a pre-model refusal names its own status without a name' \
    'a parser refusal names the field the body carried' \
    'a hostile name is echoed with its control byte made visible' \
    'a buffer that cannot hold the whole record takes nothing' \
    'a refused take drops nothing' \
    'the whole record is still there for a larger buffer'; do
    grep -Fq "$item504e_proof" tests/appliance-route-config-host.c || {
        echo "ITEM-504E: host-проба не утверждает: $item504e_proof" >&2
        exit 1
    }
done

# ------------------------------------------------------------- the QEMU proof --
grep -Fq 'the duplicate stage is refused by name' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: QEMU-проба не отказывает дублю маршрута" >&2
    exit 1
}
grep -Fq 'ROUTING_RECORD_APPLIED=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: QEMU-проба не требует применённой записи" >&2
    exit 1
}
grep -Fq 'ROUTING_RECORD_REFUSED=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: QEMU-проба не требует записи отказа" >&2
    exit 1
}
grep -Fq 'result=APPLIED' scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: QEMU-проба не читает результат применения" >&2
    exit 1
}
grep -Fq 'result=REFUSED reason=DUPLICATE_NAME' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: QEMU-проба не читает причину отказа" >&2
    exit 1
}
grep -Fq 'published no applied [PRODUCTION-ROUTE] record' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: отсутствие записи не является отказом пробы" >&2
    exit 1
}
grep -Fq 'published no refused [PRODUCTION-ROUTE] record' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504E: отсутствие записи отказа не является отказом пробы" >&2
    exit 1
}

# -------------------------------------------------------------- documents ----
grep -Fq 'PRODUCTION-ROUTE' docs/ARCHITECTURE.md || {
    echo "ITEM-504E: ARCHITECTURE не описывает запись маршрутов" >&2
    exit 1
}
grep -Fq 'PRODUCTION-ROUTE' docs/ROADMAP.md || {
    echo "ITEM-504E: ROADMAP не называет запись маршрутов" >&2
    exit 1
}
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504E: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'result=APPLIED' docs/hfoc-management-web-ui.md || {
    echo "ITEM-504E: документ панели не называет запись" >&2
    exit 1
}
