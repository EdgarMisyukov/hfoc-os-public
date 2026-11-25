#!/usr/bin/env bash
# ITEM-619 follow-up: the cooperative credential job's liveness proof must
# bracket a real production KDF, not a finished request or a client thread.
# Sourced in pinned order by scripts/check.sh; lint it on its own.

for liveness_file in \
    kernel/include/appliance_panel_auth_trace.h \
    kernel/src/appliance_panel_auth_trace.c \
    tests/panel-auth-trace-host.c \
    scripts/test-qemu-production-service-https.sh; do
    [[ -f $liveness_file ]] || {
        echo "ITEM-619: отсутствует $liveness_file" >&2
        exit 1
    }
done

# The trace owns one bounded emission point; the kernel binds it to serial.
for liveness_symbol in \
    'appliance_panel_auth_trace_sink' \
    'appliance_panel_auth_trace_set_sink(' \
    'panel_auth_emit();'; do
    grep -Fq -- "$liveness_symbol" \
        kernel/include/appliance_panel_auth_trace.h \
        kernel/src/appliance_panel_auth_trace.c || {
        echo "ITEM-619: трасса не публикует состояние:" "$liveness_symbol" >&2
        exit 1
    }
done
grep -Fq 'appliance_panel_auth_trace_set_sink(kernel_panel_auth_serial_sink);' \
    kernel/src/kernel.c || {
    echo "ITEM-619: обычная загрузка не подключает serial-приёмник трассы" >&2
    exit 1
}
grep -Fq 'serial_sequence_begin()' kernel/src/kernel.c || {
    echo "ITEM-619: serial-запись трассы не атомарна" >&2
    exit 1
}

# The production client must see the start before the probes, require measured
# KDF progress, see the finish only after them, time every /health request and
# refuse a short, finished or merely printed job.
for liveness_proof in \
    'def serial_tail(offset):' \
    'def health_probe(deadline=2.0):' \
    'def open_health(deadline=2.0):' \
    'def health_on(tls, deadline=2.0):' \
    'def kdf_progress(text):' \
    'iterations_done=' \
    'iterations_total=' \
    'the production KDF published no measurable progress' \
    'state=IN_PROGRESS' \
    'state=COMPLETED' \
    'the panel sign-in finished before any liveness probe' \
    'pending_ms < 1000' \
    'max_latency' \
    '/health returned %d'; do
    grep -Fq -- "$liveness_proof" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-619: production-проба не утверждает:" "$liveness_proof" >&2
        exit 1
    }
done
grep -Fq -- "HFOC_PROOF_SERIAL_LOG=\"\$ROUTING_SERIAL\"" \
    scripts/test-qemu-production-service-https.sh || {
    echo "ITEM-619: routing-проба не получает свой serial log" >&2
    exit 1
}
# The old completed-thread acceptance must not return under a new name.
if grep -Fq 'job already finished' \
        scripts/test-qemu-production-service-https.sh; then
    echo "ITEM-619: вернулось принятие уже завершённого входа" >&2
    exit 1
fi
# A doubled backslash sends literal \r\n instead of the HTTP line ending; that
# made the old probe time out on its own request and is not evidence about the
# appliance or the loop.
for broken_request in \
    'GET /health HTTP/1.1\\r\\n' \
    'POST / HTTP/1.1\\r\\n'; do
    if grep -Fq -- "$broken_request" \
            scripts/test-qemu-production-service-https.sh; then
        printf '%s\n' \
            "ITEM-619: проба отправляет буквальный \\r\\n вместо CRLF: $broken_request" >&2
        exit 1
    fi
done
# The allocator scope is per operation: two live connections must be able to
# hold their own scopes while one sign-in waits for the cooperative job.
grep -Fq 'test_connections_share_pool_concurrently' \
    tests/mbedtls-alloc-host.c || {
    echo "ITEM-619: host-проба не проверяет два живых TLS-соединения" >&2
    exit 1
}
grep -Fq 'appliance_production_serving_ssl_scope_enter' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-619: serving не входит в scope соединения на операцию" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PANEL_AUTH_RESULT_CANCELLED' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-619: отмена job не публикует CANCELLED" >&2
    exit 1
}
grep -Fq 'result=CANCELLED' tests/panel-auth-trace-host.c || {
    echo "ITEM-619: host-проба не проверяет CANCELLED" >&2
    exit 1
}

# The host proof drives the same sink the kernel binds.
for liveness_host in \
    'serial_capture' \
    'appliance_panel_auth_trace_set_sink(serial_capture);' \
    'the serial sink saw'; do
    grep -Fq -- "$liveness_host" tests/panel-auth-trace-host.c || {
        echo "ITEM-619: host-проба не проверяет serial sink:" "$liveness_host" >&2
        exit 1
    }
done
