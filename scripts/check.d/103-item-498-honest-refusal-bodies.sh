#!/usr/bin/env bash
# ITEM-498: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The refusal body names its status and its reason.
#
# Every refusal used to carry the same word - `rejected` - under
# `Content-Type: application/json`.  The owner's browser answered a refusal it
# could not parse, and a program could not tell a 404 from a 503 without the
# status line.  This slice pins the honest bodies and, just as important, the
# property the item must not trade away: the decision itself is unchanged.
#
#   * two pure builders - valid JSON naming status and reason, and a small HTML
#     document naming the status and linking to the panel sign-in page;
#   * the family rule that picks one of them from the request target;
#   * the serving path that builds the body for the target and no longer knows
#     the frozen word;
#   * the host proof, the seven QEMU refusal proofs and the documents.

for item498_file in \
    kernel/include/http_error_matrix.h \
    kernel/src/http_error_matrix.c \
    kernel/src/appliance_production_serving.c \
    kernel/src/virtio_net_frame_qemu.c \
    tests/http-error-matrix-host.c \
    scripts/test-qemu-https-service.sh \
    scripts/test-qemu-https-malformed-request-matrix.sh \
    scripts/test-qemu-https-size-boundary-matrix.sh \
    scripts/test-qemu-https-exhaustion-matrix.sh \
    scripts/test-qemu-https-restart-soak.sh \
    scripts/test-qemu-entropy-fail-closed-matrix.sh \
    scripts/test-qemu-management-plane-acceptance.sh \
    scripts/test-qemu-https-response-matrix.sh \
    docs/ARCHITECTURE.md \
    docs/ROADMAP.md; do
    test -f "$item498_file" || {
        echo "ITEM-498: отсутствует файл: $item498_file" >&2
        exit 1
    }
done

# 1. The two builders and the family rule.  Every status the serving path can
#    answer has a stable reason word.
grep -Fq 'http_error_matrix_json_body' kernel/include/http_error_matrix.h \
    kernel/src/http_error_matrix.c || {
    echo "ITEM-498: нет построителя JSON-тела отказа" >&2
    exit 1
}
grep -Fq 'http_error_matrix_html_body' kernel/include/http_error_matrix.h \
    kernel/src/http_error_matrix.c || {
    echo "ITEM-498: нет построителя HTML-тела отказа" >&2
    exit 1
}
grep -Fq 'http_error_matrix_body_family' kernel/include/http_error_matrix.h \
    kernel/src/http_error_matrix.c || {
    echo "ITEM-498: семейство тела не выбирается по цели" >&2
    exit 1
}
for item498_reason in \
    '"bad_request"' '"unauthorized"' '"forbidden"' '"not_found"' \
    '"method_not_allowed"' '"payload_too_large"' '"uri_too_long"' \
    '"too_many_requests"' '"header_fields_too_large"' '"unavailable"'; do
    grep -Fq "$item498_reason" kernel/src/http_error_matrix.c || {
        echo "ITEM-498: причина отказа $item498_reason не названа" >&2
        exit 1
    }
done
grep -Fq '"{\"status\":"' kernel/src/http_error_matrix.c || {
    echo "ITEM-498: JSON-тело не называет статус" >&2
    exit 1
}
grep -Fq '<!doctype html>' kernel/src/http_error_matrix.c || {
    echo "ITEM-498: HTML-тело не является документом" >&2
    exit 1
}
grep -Fq 'href=\"/panel\"' kernel/src/http_error_matrix.c || {
    echo "ITEM-498: HTML-тело не ведёт на вход в панель" >&2
    exit 1
}
# The browser family: the root, the liveness target and the panel with its
# sections; a lookalike target must not be dragged in.
for item498_browser in 'health' 'panel' 'HTTP_ERROR_BODY_HTML'; do
    grep -Fq "$item498_browser" kernel/src/http_error_matrix.c || {
        echo "ITEM-498: классификатор не знает $item498_browser" >&2
        exit 1
    }
done

# 2. The serving path builds the body for the target; the frozen word is gone.
grep -Fq 'http_error_matrix_json_body(status, body, sizeof(body))' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-498: путь обслуживания не собирает JSON-тело" >&2
    exit 1
}
grep -Fq 'http_error_matrix_html_body(status, body, sizeof(body))' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-498: путь обслуживания не собирает HTML-тело" >&2
    exit 1
}
grep -Fq 'http_error_matrix_body_family(target, target_length)' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-498: тело отказа не зависит от цели" >&2
    exit 1
}
grep -Fq 'HTTP_CONTENT_TYPE_HTML_UTF8' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-498: HTML-отказ не объявляет свой тип содержимого" >&2
    exit 1
}
# A parsed request passes its target; the pre-parse refusals have none.
grep -Fq 'line.target,' kernel/src/appliance_production_serving.c || {
    echo "ITEM-498: цель запроса не доходит до тела отказа" >&2
    exit 1
}
grep -Fq 'line->target,' kernel/src/appliance_production_serving.c || {
    echo "ITEM-498: цель запроса не доходит до тела отказа в API" >&2
    exit 1
}
if grep -Fq 'APPLIANCE_PRODUCTION_SERVING_REJECTED_BODY' \
    kernel/src/appliance_production_serving.c; then
    echo "ITEM-498: замороженное слово всё ещё в пути обслуживания" >&2
    exit 1
fi
if grep -Fq '"rejected"' kernel/src/appliance_production_serving.c; then
    echo "ITEM-498: замороженное тело всё ещё в пути обслуживания" >&2
    exit 1
fi
# The QEMU witness answers with the same bodies, so the proofs below assert
# what a peer really receives: pre-parse refusals have no target and get JSON,
# a parsed policy denial and an unknown route carry the parsed target.
grep -Fq 'http_error_matrix_json_body(status, body, sizeof(body))' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "ITEM-498: QEMU-свидетель не собирает JSON-тело" >&2
    exit 1
}
grep -Fq 'http_error_matrix_body_family(target, target_length)' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "ITEM-498: QEMU-свидетель не выбирает тело по цели" >&2
    exit 1
}
grep -Fq 'vnf_https_qemu_error_response(connection, status, NULL, 0U)' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "ITEM-498: отказ до разбора не получает JSON" >&2
    exit 1
}
grep -Fq 'connection, status, line.target, line.target_length);' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "ITEM-498: цель запроса не доходит до QEMU-свидетеля" >&2
    exit 1
}
if grep -Fq '"rejected"' kernel/src/virtio_net_frame_qemu.c; then
    echo "ITEM-498: замороженное тело всё ещё у QEMU-свидетеля" >&2
    exit 1
fi
# The decision is untouched: the four statuses the gate answers with.
for item498_status in \
    HTTP_STATUS_401_UNAUTHORIZED \
    HTTP_STATUS_403_FORBIDDEN \
    HTTP_STATUS_404_NOT_FOUND \
    HTTP_STATUS_503_SERVICE_UNAVAILABLE; do
    grep -Fq "$item498_status" kernel/src/appliance_production_serving.c || {
        echo "ITEM-498: решение $item498_status потеряно" >&2
        exit 1
    }
done

# 3. The host proof drives both builders, the family rule and the refusal of a
#    body that does not fit.
grep -Fq 'test_refusal_bodies' tests/http-error-matrix-host.c || {
    echo "ITEM-498: хост-проба тел отказа не зарегистрирована" >&2
    exit 1
}
for item498_check in \
    'the JSON refusal body names its status and reason' \
    'the HTML refusal body offers the sign-in page' \
    'a JSON body that does not fit is refused' \
    'a panel section is a browser target'; do
    grep -Fq "$item498_check" tests/http-error-matrix-host.c || {
        echo "ITEM-498: хост-проба не требует: $item498_check" >&2
        exit 1
    }
done

# 4. The QEMU refusal proofs build the honest body instead of the frozen word.
for item498_qemu in \
    scripts/test-qemu-https-service.sh \
    scripts/test-qemu-https-malformed-request-matrix.sh \
    scripts/test-qemu-https-size-boundary-matrix.sh \
    scripts/test-qemu-https-exhaustion-matrix.sh \
    scripts/test-qemu-https-restart-soak.sh \
    scripts/test-qemu-entropy-fail-closed-matrix.sh \
    scripts/test-qemu-management-plane-acceptance.sh; do
    grep -Fq 'refusal_json(' "$item498_qemu" || {
        echo "ITEM-498: $item498_qemu не собирает честное тело отказа" >&2
        exit 1
    }
    if grep -Fq 'b"rejected"' "$item498_qemu" ||
        grep -Fq "b'rejected'" "$item498_qemu"; then
        echo "ITEM-498: $item498_qemu всё ещё ждёт замороженное тело" >&2
        exit 1
    fi
done
grep -Fq 'refusal_html(' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-498: QEMU-проба матрицы ответов не требует HTML-отказ" >&2
    exit 1
}
grep -Fq '<!doctype html>' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-498: QEMU-проба матрицы ответов не читает документ" >&2
    exit 1
}
grep -Fq '404 Not Found' scripts/test-qemu-https-response-matrix.sh || {
    echo "ITEM-498: QEMU-проба матрицы ответов не называет статус" >&2
    exit 1
}

# 5. The documents tell the truth about the refusal body.
grep -Fq 'ITEM-498' docs/ROADMAP.md || {
    echo "ITEM-498: дорожная карта не называет пункт" >&2
    exit 1
}
if grep -Eq 'rejected. body' docs/ARCHITECTURE.md; then
    echo "ITEM-498: архитектура всё ещё описывает замороженное тело" >&2
    exit 1
fi
