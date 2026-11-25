#!/usr/bin/env bash
# ITEM-504 increment 6: contracted slice of the HFOC OS check suite.  Sourced in
# pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The route table the panel configures has to survive the reboot.  The table
# lives in the state volume's own store, and the boot reads it back before
# anything can be shown or committed:
#
#   * the default layout appends an eighth 64-block store (SYSTEM_CONFIG,
#     store 3) at offset 448, so every store an older volume carries keeps its
#     offset and the new record simply reads as absent there;
#   * the record is a versioned binary value (magic, count, SHA-256 of the body
#     truncated to four bytes) and never the panel's rendering;
#   * fail-closed in both directions: a blank store is an empty table, a
#     corrupt or foreign-version record is refused whole and never partially
#     applied, and a save only reports OK after the record was read back and
#     compared byte for byte;
#   * the commit writes the candidate before it replaces the applied table, so
#     a store that cannot prove the record fails the commit instead of showing
#     a route the next boot would not carry;
#   * the boot publishes `[PRODUCTION-ROUTES] source=...` with the model's own
#     rendering of the whole table, and the QEMU proof reboots the disk after
#     the routing commit and requires the committed route back.

for item504f_file in \
    kernel/include/appliance_route_store.h \
    kernel/src/appliance_route_store.c \
    kernel/include/appliance_state_layout.h \
    kernel/src/appliance_state_layout.c \
    kernel/include/appliance_route_config.h \
    kernel/src/appliance_route_config.c \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    tests/appliance-route-store-host.c \
    scripts/test-qemu-production-service-https.sh; do
    test -f "$item504f_file" || {
        echo "ITEM-504F: отсутствует файл: $item504f_file" >&2
        exit 1
    }
done

# --------------------------------------------------------------- the layout ---
grep -Fq '#define APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES 3U' \
    kernel/include/appliance_state_layout.h || {
    echo "ITEM-504F: у таблицы маршрутов нет своего хранилища" >&2
    exit 1
}
grep -Fq '#define APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS 512U' \
    kernel/include/appliance_state_layout.h || {
    echo "ITEM-504F: раскладка не выросла до восьмого хранилища" >&2
    exit 1
}
# The route store is appended after the seven existing stores, so no store an
# already-written volume carries moves.
item504f_routes_line=$(grep -n 'APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES' \
    kernel/src/appliance_state_layout.c | tail -1 | cut -d: -f1)
item504f_recovery_line=$(grep -n 'APPLIANCE_STATE_STORE_ID_RECOVERY_METADATA' \
    kernel/src/appliance_state_layout.c | tail -1 | cut -d: -f1)
if (( item504f_routes_line < item504f_recovery_line )); then
    echo "ITEM-504F: хранилище маршрутов вставлено до существующих" >&2
    exit 1
fi

# ---------------------------------------------------------------- the store ---
for item504f_api in \
    APPLIANCE_ROUTE_STORE_MAGIC \
    APPLIANCE_ROUTE_STORE_RECORD_VERSION \
    APPLIANCE_ROUTE_STORE_RECORD_BLOCKS \
    APPLIANCE_ROUTE_STORE_BODY_MAX \
    APPLIANCE_ROUTE_STORE_SOURCE_COMMITTED \
    APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING \
    APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID \
    APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_UNAVAILABLE \
    APPLIANCE_ROUTE_STORE_RECORD_CHECKSUM \
    APPLIANCE_ROUTE_STORE_WRITE_FAILED \
    appliance_route_store_load \
    appliance_route_store_save \
    appliance_route_store_source_string \
    appliance_route_store_status_string; do
    grep -Fq "$item504f_api" kernel/include/appliance_route_store.h || {
        echo "ITEM-504F: хранилище маршрутов не публикует: $item504f_api" >&2
        exit 1
    }
done

# The record is versioned, checksummed and validated by the model itself.
grep -Fq 'appliance_route_store_put_u32(record, APPLIANCE_ROUTE_STORE_MAGIC)' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: запись не несёт магию" >&2
    exit 1
}
grep -Fq 'sha256_compute(' kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: у записи нет контрольной суммы" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_STORE_RECORD_CHECKSUM' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: несовпадение контрольной суммы не различается" >&2
    exit 1
}
grep -Fq 'appliance_route_table_validate(table_out)' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: прочитанная таблица не проверяется моделью" >&2
    exit 1
}
grep -Fq 'appliance_route_store_record_is_blank(record)' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: пустое хранилище не отличается от порчи" >&2
    exit 1
}
# A store the installer or the ITEM-502 erase rebuilt carries an empty data
# region, which is "no routes yet" and not a corrupt record.
grep -Fq 'data_region_discover(&store->store.device' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: пересобранный пустой регион не распознаётся" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_MISSING' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: отсутствие записи не называется" >&2
    exit 1
}
grep -Fq 'APPLIANCE_ROUTE_STORE_SOURCE_DEFAULT_INVALID' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: испорченная запись не называется" >&2
    exit 1
}
# The write is proven by a read-back before it is reported as taken.
grep -Fq 'memcmp(verify, record, sizeof(record)) != 0' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: запись не проверяется обратным чтением" >&2
    exit 1
}
# A sealed volume is never written in the clear.
grep -Fq 'loader->encryption_sealed' kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: запечатанный том не отвергается" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: хранилище маршрутов не привязано к своему namespace" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES' \
    kernel/src/appliance_route_store.c || {
    echo "ITEM-504F: хранилище маршрутов не привязано к своему store" >&2
    exit 1
}

# ------------------------------------------------------------- the commit -----
# The candidate is durable before it is applied; the commit never applies a
# table the store did not take.
grep -Fq 'appliance_route_config_candidate(&serving->route_config)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504F: коммит не берёт кандидата у композиции" >&2
    exit 1
}
grep -Fq 'appliance_route_store_save(&serving->route_store, candidate)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504F: коммит не пишет кандидата в хранилище" >&2
    exit 1
}
item504f_save_line=$(grep -n 'appliance_route_store_save(&serving->route_store, candidate)' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
item504f_commit_line=$(grep -n 'appliance_route_config_commit(&serving->route_config)' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
if (( item504f_save_line > item504f_commit_line )); then
    echo "ITEM-504F: коммит применяет таблицу до её записи" >&2
    exit 1
fi
grep -Fq 'appliance_route_config_restore(' \
    kernel/include/appliance_route_config.h \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504F: загрузка не восстанавливает применённую таблицу" >&2
    exit 1
}
grep -Fq 'appliance_state_loader *loader' \
    kernel/include/appliance_route_store.h || {
    echo "ITEM-504F: хранилище не берёт том у загрузчика состояния" >&2
    exit 1
}

# ------------------------------------------------------------- the boot -------
grep -Fq 'static void log_production_routes(void)' kernel/src/kernel.c || {
    echo "ITEM-504F: цикл загрузки не публикует восстановленную таблицу" >&2
    exit 1
}
grep -Fq '[PRODUCTION-ROUTES] ' kernel/src/kernel.c || {
    echo "ITEM-504F: восстановление не называет свой маркер" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_take_route_restore_record(' \
    kernel/src/kernel.c \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504F: путь обслуживания не отдаёт запись восстановления" >&2
    exit 1
}
grep -Fq 'log_production_routes();' kernel/src/kernel.c || {
    echo "ITEM-504F: восстановление не вызывается из цикла" >&2
    exit 1
}
grep -Fq 'appliance_route_table_render(applied, output + length,' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-504F: запись восстановления не рендерит таблицу моделью" >&2
    exit 1
}

# ------------------------------------------------------------- the host proof --
for item504f_proof in \
    'the default layout carries eight stores' \
    'the default layout reserves 512 blocks' \
    'the route store is appended after the seven existing stores' \
    'a blank store is an empty table, not a fault' \
    'a rebuilt empty data region is an empty table, not corruption' \
    'the saved table loads back whole' \
    'the rendering separates routes with a semicolon and a space' \
    'the panel route keeps its name, backend, port, TLS and source' \
    'an empty table is a legal record' \
    'an explicitly empty table is committed, not missing' \
    'a body that disagrees with its checksum is refused whole' \
    'a record whose magic is gone is refused' \
    'a record of another version is refused' \
    'a count beyond the model'"'"'s limit is refused' \
    'a length that overruns the record is refused before it is read' \
    'a write the device refuses fails the save' \
    'the refused write left no record behind' \
    'a read-back that disagrees fails the save' \
    'no state volume means no stored routes, honestly reported' \
    'a commit cannot reach a store that was never bound' \
    'a stored table becomes the applied table' \
    'a table the model refuses leaves the applied table untouched'; do
    grep -Fq "$item504f_proof" tests/appliance-route-store-host.c || {
        echo "ITEM-504F: host-проба не утверждает: $item504f_proof" >&2
        exit 1
    }
done
grep -Fq 'test-appliance-route-store-host' Makefile || {
    echo "ITEM-504F: host-проба не собрана в Makefile" >&2
    exit 1
}
# shellcheck disable=SC2016  # the compared text is literal, not expanded
grep -Fq '$(MAKE) test-appliance-route-store-host' Makefile || {
    echo "ITEM-504F: host-проба не входит в host suite" >&2
    exit 1
}

# ------------------------------------------------------------- the QEMU proof --
grep -Fq 'ROUTE_RESTORE_RECORD=' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504F: QEMU-проба не требует восстановления маршрута" >&2
    exit 1
}
# ITEM-504 increment 9: the restore record now carries the three committed
# routes (a.example, b.example and the second relay route c.example), so this
# pin names all of them.
# shellcheck disable=SC2016  # the pinned text is the QEMU script's own source
grep -Fq 'source=COMMITTED routes=3; name=a.example backend=192.0.2.10:443 tls=on source=PANEL; name=b.example backend=10.0.2.2:$FACADE_BACKEND_PORT tls=off source=PANEL; name=c.example backend=10.0.2.2:$FACADE_BACKEND2_PORT tls=off source=PANEL' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504F: QEMU-проба не называет восстановленные маршруты" >&2
    exit 1
}
grep -Fq 'The rebooted appliance did not carry the committed route' \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-504F: отсутствие маршрута после перезагрузки не является отказом" >&2
    exit 1
}

# -------------------------------------------------------------- documents ----
grep -Fq 'PRODUCTION-ROUTES' docs/ARCHITECTURE.md || {
    echo "ITEM-504F: ARCHITECTURE не описывает восстановление таблицы" >&2
    exit 1
}
grep -Fq 'PRODUCTION-ROUTES' docs/ROADMAP.md || {
    echo "ITEM-504F: ROADMAP не называет восстановление таблицы" >&2
    exit 1
}
grep -Fq 'ITEM-504' docs/ROADMAP.md || {
    echo "ITEM-504F: ROADMAP не называет пункт" >&2
    exit 1
}
grep -Fq 'DEFAULT_MISSING' docs/hfoc-management-web-ui.md || {
    echo "ITEM-504F: документ панели не различает применённое и сохранённое" >&2
    exit 1
}
