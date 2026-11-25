#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# ITEM-378 signed diagnostic bundle, consent session and break-glass contracts.
for support378_target in test-appliance-support-bundle-host \
                         test-appliance-support-session-host \
                         test-appliance-support-break-glass-host \
                         test-appliance-production-support-host; do
    grep -Fq "${support378_target}:" Makefile || {
        echo "Makefile не содержит ${support378_target}" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) ${support378_target}" Makefile || {
        echo "test: target не содержит ${support378_target}" >&2
        exit 1
    }
done
for support378_header in \
    'kernel/include/appliance_support_bundle.h:APPLIANCE_SUPPORT_BUNDLE_SCHEMA_ID' \
    'kernel/include/appliance_support_bundle.h:appliance_support_bundle_seal' \
    'kernel/include/appliance_support_bundle.h:appliance_support_bundle_verify' \
    'kernel/include/appliance_support_session.h:APPLIANCE_SUPPORT_SESSION_SCHEMA_ID' \
    'kernel/include/appliance_support_session.h:APPLIANCE_SUPPORT_SESSION_SCOPE_SHELL' \
    'kernel/include/appliance_support_session.h:appliance_support_session_consent' \
    'kernel/include/appliance_support_break_glass.h:APPLIANCE_SUPPORT_BREAK_GLASS_SCHEMA_ID' \
    'kernel/include/appliance_support_break_glass.h:APPLIANCE_SUPPORT_BREAK_GLASS_ACTION_SHELL' \
    'kernel/include/appliance_support_break_glass.h:appliance_support_break_glass_apply' \
    'kernel/include/appliance_production_support.h:APPLIANCE_PRODUCTION_SUPPORT_SCHEMA_ID' \
    'kernel/include/appliance_production_support.h:appliance_production_support_publish_bundle'; do
    support378_file=${support378_header%%:*}
    support378_symbol=${support378_header##*:}
    grep -Fq "$support378_symbol" "$support378_file" || {
        echo "$support378_file не содержит $support378_symbol" >&2
        exit 1
    }
done
for support378_symbol in \
    appliance_support_bundle_seal \
    appliance_support_bundle_verify \
    appliance_support_bundle_state_string \
    appliance_support_session_consent \
    appliance_support_session_state_string \
    appliance_support_break_glass_apply \
    appliance_support_break_glass_state_string \
    appliance_production_support_init \
    appliance_production_support_publish_bundle \
    appliance_production_support_render; do
    nm build/hfoc-kernel.elf | grep -F " $support378_symbol" >/dev/null || {
        echo "Production kernel не содержит $support378_symbol" >&2
        exit 1
    }
done
for support378_source in kernel/src/appliance_support_bundle.c \
                        kernel/src/appliance_support_session.c \
                        kernel/src/appliance_support_break_glass.c \
                        kernel/src/appliance_production_support.c; do
    if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
            "$support378_source"; then
        echo "$support378_source выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
        exit 1
    fi
done
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Appliance support bundle host tests: ALL OK'; then
    echo "Production kernel содержит support bundle host marker" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Appliance production support host tests: ALL OK'; then
    echo "Production kernel содержит production support host marker" >&2
    exit 1
fi
for support378_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'support bundle.*(redaction|break-glass)|break-glass recovery' \
            "$support378_doc"; then
        echo "$support378_doc не документирует ITEM-378" >&2
        exit 1
    fi
done
grep -Fq 'HFOC-PRODUCTION-SUPPORT-1' \
    docs/hfoc-support-bundle-redaction-break-glass.md || {
    echo "ITEM-378 документ не содержит канонический schema id" >&2
    exit 1
}

# Supportability QEMU proof contracts.
grep -Fq 'test-supportability-qemu' Makefile || {
    echo "Makefile не содержит test-supportability-qemu" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-supportability-qemu' Makefile || {
    echo "test: target не содержит test-supportability-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-qemu-supportability.sh' Makefile || {
    echo "Makefile не содержит supportability QEMU script" >&2
    exit 1
}
grep -Fq 'HFOC_SUPPORTABILITY_QEMU_TEST' \
    kernel/src/supportability_qemu.c || {
    echo "supportability_qemu.c не имеет proof guard" >&2
    exit 1
}
grep -Fq '[SUPPORTABILITY] proof init' \
    kernel/src/supportability_qemu.c || {
    echo "supportability_qemu.c не содержит proof init marker" >&2
    exit 1
}
grep -Fq 'supportability_qemu_test_init' kernel/src/kernel.c || {
    echo "kernel.c не вызывает supportability proof" >&2
    exit 1
}
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf '[SUPPORTABILITY]'; then
    echo "Production kernel содержит supportability marker" >&2
    exit 1
fi
if grep -Fq 'qemu-supportability' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит supportability script" >&2
    exit 1
fi
grep -Fq 'QEMU supportability baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-supportability.sh || {
    echo "test-qemu-supportability.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU supportability test: OK' \
    scripts/test-qemu-supportability.sh || {
    echo "test-qemu-supportability.sh не содержит success marker" >&2
    exit 1
}
for supportability_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'supportability QEMU proof|supportability-qemu QEMU proof' \
            "$supportability_doc"; then
        echo "$supportability_doc не документирует supportability QEMU proof" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **170. Supportability QEMU proof' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 170 завершённым" >&2
    exit 1
}

# External HFOC accounting integration boundary contracts.
grep -Fq 'test-integration-boundary-host' Makefile || {
    echo "Makefile не содержит test-integration-boundary-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-boundary-host' Makefile || {
    echo "test: target не содержит test-integration-boundary-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_BOUNDARY_SCHEMA_ID "HFOC-ACCT-INT-1"' \
    kernel/include/integration_boundary.h || {
    echo "integration_boundary.h не содержит exact schema ID" >&2
    exit 1
}
grep -Fq 'INTEGRATION_BOUNDARY_OWNER_APPLIANCE 1U' \
    kernel/include/integration_boundary.h || {
    echo "integration_boundary.h не содержит appliance owner" >&2
    exit 1
}
grep -Fq 'INTEGRATION_BOUNDARY_OWNER_ACCOUNTING 2U' \
    kernel/include/integration_boundary.h || {
    echo "integration_boundary.h не содержит accounting owner" >&2
    exit 1
}
grep -Fq 'INTEGRATION_BOUNDARY_AUDIT_HANDOFF_VERSION 1U' \
    kernel/include/integration_boundary.h || {
    echo "integration_boundary.h не содержит audit hand-off version" >&2
    exit 1
}
grep -Fq 'INTEGRATION_BOUNDARY_TRANSPORT_VERSION 1U' \
    kernel/include/integration_boundary.h || {
    echo "integration_boundary.h не содержит transport version" >&2
    exit 1
}
grep -Fq 'INTEGRATION_BOUNDARY_UNSUPPORTED_ARBITRARY_SHELL' \
    kernel/include/integration_boundary.h || {
    echo "integration_boundary.h не содержит unsupported shell bit" >&2
    exit 1
}
grep -Fq 'Integration boundary host tests: ALL OK' \
    tests/integration-boundary-host.c || {
    echo "integration-boundary host harness не содержит success marker" >&2
    exit 1
}
for integration_section in '## Contract Identity' '## Ownership Split' \
                          '## Audit Hand-off' '## Transport' \
                          '## Versioning' '## Unsupported Operations'; do
    grep -Fq "$integration_section" docs/ACCOUNTING-INTEGRATION.md || {
        echo "ACCOUNTING-INTEGRATION.md не содержит $integration_section" >&2
        exit 1
    }
done
if ! printf '#include "integration_boundary.h"\nvoid hfoc_integration_boundary_header(void) { (void)INTEGRATION_BOUNDARY_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_boundary.h не компилируется как self-contained header" >&2
    exit 1
fi
for integration_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'external HFOC accounting integration boundary|integration boundary' \
            "$integration_doc"; then
        echo "$integration_doc не документирует integration boundary" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **171. Integration boundary document' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 171 завершённым" >&2
    exit 1
}

# External HFOC accounting integration principal scope contracts.
grep -Fq 'test-integration-principal-host' Makefile || {
    echo "Makefile не содержит test-integration-principal-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-principal-host' Makefile || {
    echo "test: target не содержит test-integration-principal-host" >&2
    exit 1
}
grep -Fq 'AUTHORIZATION_ROLE_INTEGRATION 4U' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит AUTHORIZATION_ROLE_INTEGRATION" >&2
    exit 1
}
grep -Fq 'AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION 4U' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит INTEGRATION_SESSION capability" >&2
    exit 1
}
grep -Fq 'INTEGRATION_PRINCIPAL_VERSION' \
    kernel/include/integration_principal.h || {
    echo "integration_principal.h не содержит INTEGRATION_PRINCIPAL_VERSION" >&2
    exit 1
}
grep -Fq 'struct integration_principal_scope' \
    kernel/include/integration_principal.h || {
    echo "integration_principal.h не содержит integration scope descriptor" >&2
    exit 1
}
for integration_principal_symbol in integration_principal_scope_validate \
                                   integration_principal_scope_init \
                                   integration_principal_scope_check \
                                   integration_principal_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_principal_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_principal_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_principal.c; then
    echo "integration_principal.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_principal_host_test_'; then
    echo "integration_principal_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration principal host tests: ALL OK'; then
    echo "Production kernel содержит integration principal host marker" >&2
    exit 1
fi
if ! printf '#include "integration_principal.h"\nvoid hfoc_integration_principal_header(void) { (void)INTEGRATION_PRINCIPAL_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_principal.h не компилируется как self-contained header" >&2
    exit 1
fi
for integration_principal_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'external integration principal scope|integration principal scope' \
            "$integration_principal_doc"; then
        echo "$integration_principal_doc не документирует integration principal scope" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **172. Integration principal role' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 172 завершённым" >&2
    exit 1
}

# External HFOC accounting integration session handshake contracts.
grep -Fq 'test-integration-session-host' Makefile || {
    echo "Makefile не содержит test-integration-session-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-session-host' Makefile || {
    echo "test: target не содержит test-integration-session-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_SESSION_VERSION' \
    kernel/include/integration_session.h || {
    echo "integration_session.h не содержит INTEGRATION_SESSION_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_SESSION_NONCE_SIZE' \
    kernel/include/integration_session.h || {
    echo "integration_session.h не содержит INTEGRATION_SESSION_NONCE_SIZE" >&2
    exit 1
}
grep -Fq 'INTEGRATION_SESSION_NONCE_REGISTRY_MAX' \
    kernel/include/integration_session.h || {
    echo "integration_session.h не содержит nonce registry maximum" >&2
    exit 1
}
grep -Fq 'struct integration_session_request' \
    kernel/include/integration_session.h || {
    echo "integration_session.h не содержит handshake request descriptor" >&2
    exit 1
}
grep -Fq 'struct integration_session_nonce_registry' \
    kernel/include/integration_session.h || {
    echo "integration_session.h не содержит nonce registry descriptor" >&2
    exit 1
}
for integration_session_symbol in \
        integration_session_nonce_registry_init \
        integration_session_handshake_validate \
        integration_session_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_session_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_session_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_session.c; then
    echo "integration_session.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_session_host_test_'; then
    echo "integration_session_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration session host tests: ALL OK'; then
    echo "Production kernel содержит integration session host marker" >&2
    exit 1
fi
if ! printf '#include "integration_session.h"\nvoid hfoc_integration_session_header(void) { (void)INTEGRATION_SESSION_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_session.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Integration Session Handshake' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит integration session handshake section" >&2
    exit 1
}
for integration_session_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'integration session handshake' "$integration_session_doc"; then
        echo "$integration_session_doc не документирует integration session handshake" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **173. Integration session handshake' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 173 завершённым" >&2
    exit 1
}

# External HFOC accounting hand-off envelope v1 contracts.
grep -Fq 'test-integration-envelope-host' Makefile || {
    echo "Makefile не содержит test-integration-envelope-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-envelope-host' Makefile || {
    echo "test: target не содержит test-integration-envelope-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_ENVELOPE_VERSION' \
    kernel/include/integration_envelope.h || {
    echo "integration_envelope.h не содержит INTEGRATION_ENVELOPE_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_ENVELOPE_ID_SIZE' \
    kernel/include/integration_envelope.h || {
    echo "integration_envelope.h не содержит envelope ID size" >&2
    exit 1
}
grep -Fq 'INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE' \
    kernel/include/integration_envelope.h || {
    echo "integration_envelope.h не содержит correlation ID size" >&2
    exit 1
}
grep -Fq 'INTEGRATION_ENVELOPE_PAYLOAD_LENGTH_MAX' \
    kernel/include/integration_envelope.h || {
    echo "integration_envelope.h не содержит payload length maximum" >&2
    exit 1
}
grep -Fq 'INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF' \
    kernel/include/integration_envelope.h || {
    echo "integration_envelope.h не содержит audit hand-off purpose" >&2
    exit 1
}
grep -Fq 'struct integration_envelope_layout' \
    kernel/include/integration_envelope.h || {
    echo "integration_envelope.h не содержит layout descriptor" >&2
    exit 1
}
for integration_envelope_symbol in \
        integration_envelope_seal \
        integration_envelope_validate \
        integration_envelope_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_envelope_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_envelope_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_envelope.c; then
    echo "integration_envelope.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_envelope_host_test_'; then
    echo "integration_envelope_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration envelope host tests: ALL OK'; then
    echo "Production kernel содержит integration envelope host marker" >&2
    exit 1
fi
if ! printf '#include "integration_envelope.h"\nvoid hfoc_integration_envelope_header(void) { (void)INTEGRATION_ENVELOPE_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_envelope.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Accounting Hand-off Envelope v1' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит hand-off envelope section" >&2
    exit 1
}
for integration_envelope_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'accounting hand-off envelope|hand-off envelope' \
            "$integration_envelope_doc"; then
        echo "$integration_envelope_doc не документирует hand-off envelope" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **174. Accounting hand-off envelope v1' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 174 завершённым" >&2
    exit 1
}

# External HFOC accounting idempotency ledger contracts.
grep -Fq 'test-integration-ledger-host' Makefile || {
    echo "Makefile не содержит test-integration-ledger-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-ledger-host' Makefile || {
    echo "test: target не содержит test-integration-ledger-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_LEDGER_VERSION' \
    kernel/include/integration_ledger.h || {
    echo "integration_ledger.h не содержит INTEGRATION_LEDGER_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_LEDGER_IDENTITY_SIZE' \
    kernel/include/integration_ledger.h || {
    echo "integration_ledger.h не содержит identity size" >&2
    exit 1
}
grep -Fq 'INTEGRATION_LEDGER_MAX_ENTRIES' \
    kernel/include/integration_ledger.h || {
    echo "integration_ledger.h не содержит maximum entries" >&2
    exit 1
}
grep -Fq 'struct integration_ledger_entry' \
    kernel/include/integration_ledger.h || {
    echo "integration_ledger.h не содержит entry descriptor" >&2
    exit 1
}
grep -Fq 'struct integration_ledger' \
    kernel/include/integration_ledger.h || {
    echo "integration_ledger.h не содержит ledger descriptor" >&2
    exit 1
}
for integration_ledger_symbol in \
        integration_ledger_init \
        integration_ledger_prepare \
        integration_ledger_commit \
        integration_ledger_replay \
        integration_ledger_rollback \
        integration_ledger_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_ledger_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_ledger_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_ledger.c; then
    echo "integration_ledger.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_ledger_host_test_'; then
    echo "integration_ledger_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration ledger host tests: ALL OK'; then
    echo "Production kernel содержит integration ledger host marker" >&2
    exit 1
fi
if ! printf '#include "integration_ledger.h"\nvoid hfoc_integration_ledger_header(void) { (void)INTEGRATION_LEDGER_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_ledger.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Idempotency Ledger' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит idempotency ledger section" >&2
    exit 1
}
for integration_ledger_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'idempotency ledger' "$integration_ledger_doc"; then
        echo "$integration_ledger_doc не документирует idempotency ledger" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **175. Idempotency ledger' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 175 завершённым" >&2
    exit 1
}

# External HFOC accounting outbound event cursor contracts.
grep -Fq 'test-integration-event-cursor-host' Makefile || {
    echo "Makefile не содержит test-integration-event-cursor-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-event-cursor-host' Makefile || {
    echo "test: target не содержит test-integration-event-cursor-host" >&2
    exit 1
}
grep -Fq 'INTEGRATION_EVENT_CURSOR_VERSION' \
    kernel/include/integration_event_cursor.h || {
    echo "integration_event_cursor.h не содержит INTEGRATION_EVENT_CURSOR_VERSION" >&2
    exit 1
}
grep -Fq 'INTEGRATION_EVENT_CURSOR_MAX_RECORDS' \
    kernel/include/integration_event_cursor.h || {
    echo "integration_event_cursor.h не содержит maximum records" >&2
    exit 1
}
grep -Fq 'struct integration_event_cursor_record' \
    kernel/include/integration_event_cursor.h || {
    echo "integration_event_cursor.h не содержит record descriptor" >&2
    exit 1
}
grep -Fq 'struct integration_event_cursor' \
    kernel/include/integration_event_cursor.h || {
    echo "integration_event_cursor.h не содержит cursor descriptor" >&2
    exit 1
}
for integration_event_cursor_symbol in \
        integration_event_cursor_init \
        integration_event_cursor_export \
        integration_event_cursor_acknowledge \
        integration_event_cursor_snapshot \
        integration_event_cursor_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $integration_event_cursor_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $integration_event_cursor_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|serial_write|console_write|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b|\b(block_device_read|block_device_write)\b|\b(event_ring_|audit_)\b' \
        kernel/src/integration_event_cursor.c; then
    echo "integration_event_cursor.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf |
        grep 'integration_event_cursor_host_test_'; then
    echo "integration_event_cursor_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Integration event cursor host tests: ALL OK'; then
    echo "Production kernel содержит integration event cursor host marker" >&2
    exit 1
fi
if ! printf '#include "integration_event_cursor.h"\nvoid hfoc_integration_event_cursor_header(void) { (void)INTEGRATION_EVENT_CURSOR_VERSION; }\n' |
    gcc -std=c11 -Wall -Wextra -Werror -Wpedantic -ffreestanding \
        -Ikernel/include -x c -c - -o /dev/null; then
    echo "integration_event_cursor.h не компилируется как self-contained header" >&2
    exit 1
fi
grep -Fq '## Outbound Event Cursor' \
    docs/ACCOUNTING-INTEGRATION.md || {
    echo "ACCOUNTING-INTEGRATION.md не содержит outbound event cursor section" >&2
    exit 1
}
for integration_event_cursor_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'outbound event cursor' "$integration_event_cursor_doc"; then
        echo "$integration_event_cursor_doc не документирует outbound event cursor" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **176. Outbound event cursor' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 176 завершённым" >&2
    exit 1
}
