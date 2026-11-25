#!/usr/bin/env bash
# ITEM-507 increment 4: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The name -> certificate/key declaration is mutable, durable and consistent
# with the route table.
#
# Increment 3 selects a certificate by name; this increment lets the operator
# change the declaration through the ITEM-505 two-phase mutation plane.  The
# claims pinned here:
#
#   * one `CERTIFICATE_NAME` mutation (command id 16, matrix fifteen entries)
#     behind `POST /api/v1/system/certificate/name`, POST only and with no read
#     meaning, dispatched by the plane to a bound executor;
#   * stage parses `action=import|delete|default`, decodes the percent-encoded
#     PEM pair and validates it through the ITEM-503 import gate against the
#     declared name, so a name can never point at somebody else's certificate;
#     the private key candidate is zeroed on every path;
#   * commit builds on the saved table (not on the boot-time table the listener
#     serves), writes the whole record with a read-back and answers APPLIED only
#     then; the record `[PRODUCTION-CERTIFICATE-NAME]` never carries key or
#     certificate bytes and says `applies=reboot`, which the running listener
#     honours;
#   * a routed name without its own certificate is refused with
#     ROUTE_WITHOUT_CERTIFICATE even when a default certificate is declared;
#   * the item 504/505/506 matrix-count pins moved with the matrix.
for item507_inc4_file in \
    kernel/include/appliance_management_authorization.h \
    kernel/src/appliance_management_authorization.c \
    kernel/include/http_parser.h \
    kernel/src/http_parser.c \
    kernel/include/appliance_production_mutation_plane.h \
    kernel/src/appliance_production_mutation_plane.c \
    kernel/include/appliance_certificate_name_store.h \
    kernel/src/appliance_certificate_name_store.c \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/src/kernel.c \
    kernel/src/appliance_management_plane_qemu.c \
    tests/appliance-production-mutation-plane-host.c \
    tests/appliance-certificate-name-store-host.c \
    tests/appliance-management-authorization-host.c \
    scripts/test-qemu-production-service-https.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    [[ -f $item507_inc4_file ]] || {
        echo "ITEM-507 inc4: отсутствует $item507_inc4_file" >&2
        exit 1
    }
done

# The authorization entry, the command id and the target.
for item507_inc4_auth in \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_CERTIFICATE_NAME_PATH' \
    '"/api/v1/system/certificate/name"' \
    '#define APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_NAME_MUTATION 16U' \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_MUTATION_CERTIFICATE_NAME = 10,'; do
    grep -Fq -- "$item507_inc4_auth" \
        kernel/include/appliance_management_authorization.h || {
        echo "ITEM-507 inc4: авторизация не называет мутацию:" \
            "$item507_inc4_auth" >&2
        exit 1
    }
done
for item507_inc4_matrix in \
    'struct authorization_entry entries[17];' \
    'index < 17U' \
    'APPLIANCE_MANAGEMENT_AUTHORIZATION_COMMAND_CERTIFICATE_NAME_MUTATION,' \
    'return "CERTIFICATE_NAME";'; do
    grep -Fq -- "$item507_inc4_matrix" \
        kernel/src/appliance_management_authorization.c || {
        echo "ITEM-507 inc4: матрица не выросла на мутацию имени:" \
            "$item507_inc4_matrix" >&2
        exit 1
    }
done
# The QEMU fixture and the earlier items' pins moved with the matrix.
grep -Fq 'policy.matrix.count == 17U' \
    kernel/src/appliance_management_plane_qemu.c || {
    echo "ITEM-507 inc4: QEMU-фикстура не знает выросшую матрицу" >&2
    exit 1
}
for item507_inc4_pin in \
    scripts/check.d/110-item-504-routing-mutation.sh \
    scripts/check.d/118-item-505-system-hostname.sh \
    scripts/check.d/125-item-506-panel-account.sh; do
    grep -Fq 'policy.matrix.count == 17U' "$item507_inc4_pin" || {
        echo "ITEM-507 inc4: пин матрицы не сдвинут в $item507_inc4_pin" >&2
        exit 1
    }
done
for item507_inc4_shape in \
    scripts/check.d/118-item-505-system-hostname.sh \
    scripts/check.d/125-item-506-panel-account.sh; do
    grep -Fq 'struct authorization_entry entries[17];' \
        "$item507_inc4_shape" || {
        echo "ITEM-507 inc4: размер матрицы не сдвинут в $item507_inc4_shape" >&2
        exit 1
    }
    grep -Fq 'index < 17U' "$item507_inc4_shape" || {
        echo "ITEM-507 inc4: заполнение матрицы не сдвинуто в $item507_inc4_shape" >&2
        exit 1
    }
done

# The HTTP policy knows the target and the form gate collects its body.
grep -Fq 'HTTP_POLICY_CERTIFICATE_NAME_TARGET_LENGTH 31U' \
    kernel/include/http_parser.h || {
    echo "ITEM-507 inc4: HTTP-политика не знает цель имени" >&2
    exit 1
}
grep -Fq 'HTTP_POLICY_CERTIFICATE_NAME_TARGET,' kernel/src/http_parser.c || {
    echo "ITEM-507 inc4: цель имени не в таблице форм" >&2
    exit 1
}

# The plane: the fifth executor, bound, refused when unbound, dispatched in all
# three phases.
for item507_inc4_plane in \
    'APPLIANCE_PRODUCTION_MUTATION_PLANE_CERTIFICATE_NAME_OK = 0,' \
    'struct appliance_production_mutation_plane_certificate_name_executor {' \
    'struct appliance_production_mutation_plane_certificate_name_executor' \
    'bool certificate_name_bound;' \
    'appliance_production_mutation_plane_bind_certificate_name(' \
    'appliance_production_mutation_plane_certificate_name_ready(' \
    'plane->certificate_name.stage(' \
    'plane->certificate_name.commit(' \
    'plane->certificate_name.rollback('; do
    grep -Fq -- "$item507_inc4_plane" \
        kernel/include/appliance_production_mutation_plane.h \
        kernel/src/appliance_production_mutation_plane.c || {
        echo "ITEM-507 inc4: плоскость мутаций не знает мутацию имени:" \
            "$item507_inc4_plane" >&2
        exit 1
    }
done

# The executor: the actions, the import gate, the saved table, the zeroing and
# the record.
for item507_inc4_serving in \
    'appliance_production_serving_certificate_name_stage' \
    'appliance_production_serving_certificate_name_commit' \
    'appliance_production_serving_certificate_name_rollback' \
    'appliance_certificate_import_validate(' \
    'appliance_production_serving_form_decode(' \
    'appliance_route_name_normalize(' \
    'serving->name_saved = table;' \
    'table = serving->name_saved;' \
    'appliance_certificate_name_store_save(' \
    'appliance_production_serving_certificate_name_reset(' \
    '"[PRODUCTION-CERTIFICATE-NAME] source=PANEL action="' \
    '" applies=reboot"' \
    'appliance_production_serving_handle_certificate_name(' \
    'appliance_production_serving_take_certificate_name_record('; do
    grep -Fq -- "$item507_inc4_serving" \
        kernel/src/appliance_production_serving.c \
        kernel/include/appliance_production_serving.h || {
        echo "ITEM-507 inc4: исполнитель не реализует:" \
            "$item507_inc4_serving" >&2
        exit 1
    }
done
# The handler must be dispatched: the executor and the plane can be perfect
# while nothing on the wire reaches them, which is exactly how this increment
# first shipped a target that answered 404.
grep -Fq 'if (appliance_production_serving_handle_certificate_name(' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-507 inc4: цель не направляется в исполнитель" >&2
    exit 1
}
grep -Fq 'static void log_production_certificate_name(void)' kernel/src/kernel.c || {
    echo "ITEM-507 inc4: цикл не публикует запись мутации" >&2
    exit 1
}
grep -Fq 'log_production_certificate_name();' kernel/src/kernel.c || {
    echo "ITEM-507 inc4: цикл не вызывает публикацию записи мутации" >&2
    exit 1
}

# Route consistency: the selection takes the route answer and has its own
# refusal reason.
for item507_inc4_route in \
    'APPLIANCE_CERTIFICATE_NAME_ROUTE_WITHOUT_CERTIFICATE = 11,' \
    'bool name_is_routed,'; do
    grep -Fq -- "$item507_inc4_route" \
        kernel/include/appliance_certificate_name_store.h || {
        echo "ITEM-507 inc4: модель не знает согласованности с маршрутами:" \
            "$item507_inc4_route" >&2
        exit 1
    }
done
grep -Fq 'ROUTE_WITHOUT_CERTIFICATE' kernel/src/appliance_certificate_name_store.c || {
    echo "ITEM-507 inc4: отказ маршрута без сертификата не назван" >&2
    exit 1
}
grep -Fq 'appliance_route_table_lookup(' kernel/src/appliance_production_serving.c || {
    echo "ITEM-507 inc4: колбэк не спрашивает таблицу маршрутов" >&2
    exit 1
}

# The proofs: the plane executor and the selection cases.
for item507_inc4_proof in \
    'certificate-name executor: an unbound mutation is refused' \
    'certificate-name executor: the executor binds' \
    'certificate-name executor: the wrong token is rejected' \
    'certificate-name executor: a failed durable write is APPLY_FAILED' \
    'certificate-name executor: the confirmed mutation applies' \
    'certificate-name executor: the explicit cancel cancels'; do
    grep -Fq -- "$item507_inc4_proof" \
        tests/appliance-production-mutation-plane-host.c || {
        echo "ITEM-507 inc4: проба плоскости не утверждает:" \
            "$item507_inc4_proof" >&2
        exit 1
    }
done
for item507_inc4_select in \
    'a routed name with its own certificate selects it' \
    'a routed name without a certificate is refused, default or not' \
    'a malformed routed name is refused for the same reason' \
    'the refusal does not depend on the default declaration'; do
    grep -Fq -- "$item507_inc4_select" \
        tests/appliance-certificate-name-store-host.c || {
        echo "ITEM-507 inc4: проба выбора не утверждает:" \
            "$item507_inc4_select" >&2
        exit 1
    }
done
grep -Fq 'policy.matrix.count == 17U' \
    tests/appliance-management-authorization-host.c || {
    echo "ITEM-507 inc4: проба авторизации не знает матрицу 16" >&2
    exit 1
}

# The QEMU proof: the mutation on a live listener and its rebooted effect.
for item507_inc4_qemu in \
    'serving-certificate-name-client.py' \
    'action=default&mode=off' \
    'action=import&name=x.example&certificate=not+a+pem&key=also+not' \
    'a malformed certificate-name import was not ' \
    'PRODUCTION-CERTIFICATE-NAME] source=PANEL action=DEFAULT name=- names=0 default=REFUSE persisted=OK applies=reboot' \
    'PRODUCTION-TLS-SNI] name=unknown.example result=REFUSED cert=none reason=NOT_FOUND' \
    'PRODUCTION-TLS-SNI] name=- result=DEFAULT cert=default'; do
    grep -Fq -- "$item507_inc4_qemu" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-507 inc4: QEMU-проба не требует:" \
            "$item507_inc4_qemu" >&2
        exit 1
    }
done

# The documents record the increment.
grep -Fq 'ROUTE_WITHOUT_CERTIFICATE' docs/ARCHITECTURE.md || {
    echo "ITEM-507 inc4: ARCHITECTURE не описывает согласованность" >&2
    exit 1
}
grep -Fq 'ITEM-507 increment 4' docs/ROADMAP.md || {
    echo "ITEM-507 inc4: ROADMAP не записывает инкремент" >&2
    exit 1
}
