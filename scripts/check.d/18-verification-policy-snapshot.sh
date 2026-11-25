#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Verification policy snapshot contracts.
grep -Fq 'test-verification-policy-host' Makefile || {
    echo "Makefile не содержит test-verification-policy-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-verification-policy-host' Makefile || {
    echo "test: target не содержит test-verification-policy-host" >&2
    exit 1
}
grep -Fq 'VERIFICATION_POLICY_VERSION' \
    kernel/include/verification_policy.h || {
    echo "verification_policy.h не содержит VERIFICATION_POLICY_VERSION" >&2
    exit 1
}
grep -Fq 'VERIFICATION_POLICY_GENERATION_MIN' \
    kernel/include/verification_policy.h || {
    echo "verification_policy.h не содержит VERIFICATION_POLICY_GENERATION_MIN" >&2
    exit 1
}
grep -Fq 'VERIFICATION_POLICY_ANCHOR_MAX' \
    kernel/include/verification_policy.h || {
    echo "verification_policy.h не содержит VERIFICATION_POLICY_ANCHOR_MAX" >&2
    exit 1
}
grep -Fq 'struct verification_policy_snapshot' \
    kernel/include/verification_policy.h || {
    echo "verification_policy.h не содержит policy snapshot" >&2
    exit 1
}
grep -Fq 'struct verification_policy' \
    kernel/include/verification_policy.h || {
    echo "verification_policy.h не содержит struct verification_policy" >&2
    exit 1
}
for policy_symbol in verification_policy_snapshot_validate \
                     verification_policy_init \
                     verification_policy_stage \
                     verification_policy_commit \
                     verification_policy_rollback \
                     verification_policy_active \
                     verification_policy_has_pending \
                     verification_policy_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $policy_symbol" >/dev/null || {
        echo "Production kernel не содержит $policy_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/verification_policy.c; then
    echo "verification_policy.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'verification_policy_host_test_'; then
    echo "verification_policy_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Verification policy host tests: ALL OK'; then
    echo "Production kernel содержит verification policy host marker" >&2
    exit 1
fi
for policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'verification policy|verification-policy' \
            "$policy_doc"; then
        echo "$policy_doc не документирует verification policy snapshots" >&2
        exit 1
    fi
done

# Key use separation contracts.
grep -Fq 'test-key-use-host' Makefile || {
    echo "Makefile не содержит test-key-use-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-key-use-host' Makefile || {
    echo "test: target не содержит test-key-use-host" >&2
    exit 1
}
for key_use_constant in \
    KEY_USE_PURPOSE_IMAGE \
    KEY_USE_PURPOSE_UPDATE \
    KEY_USE_PURPOSE_BACKUP \
    KEY_USE_PURPOSE_INTEGRATION \
    KEY_USE_PURPOSE_RECOVERY \
    KEY_USE_ACCEPTED_PURPOSES_MASK; do
    grep -Fq "$key_use_constant" kernel/include/key_use.h || {
        echo "key_use.h не содержит $key_use_constant" >&2
        exit 1
    }
done
for key_use_symbol in key_use_purpose_valid \
                      key_use_accepted_purposes_valid \
                      key_use_purpose_is_accepted; do
    nm build/hfoc-kernel.elf | grep -F " $key_use_symbol" >/dev/null || {
        echo "Production kernel не содержит $key_use_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/key_use.c; then
    echo "key_use.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'key_use_host_test_'; then
    echo "key_use_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Key use host tests: ALL OK'; then
    echo "Production kernel содержит key use host marker" >&2
    exit 1
fi
for key_use_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'key use|key-use' "$key_use_doc"; then
        echo "$key_use_doc не документирует key use separation" >&2
        exit 1
    fi
done

# Signed metadata envelope contracts.
grep -Fq 'test-signed-metadata-host' Makefile || {
    echo "Makefile не содержит test-signed-metadata-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-signed-metadata-host' Makefile || {
    echo "test: target не содержит test-signed-metadata-host" >&2
    exit 1
}
grep -Fq 'SIGNED_METADATA_FORMAT_VERSION' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит SIGNED_METADATA_FORMAT_VERSION" >&2
    exit 1
}
grep -Fq 'SIGNED_METADATA_HEADER_SIZE' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит SIGNED_METADATA_HEADER_SIZE" >&2
    exit 1
}
grep -Fq 'SIGNED_METADATA_DIGEST_SIZE' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит SIGNED_METADATA_DIGEST_SIZE" >&2
    exit 1
}
grep -Fq 'SIGNED_METADATA_PURPOSE_OFFSET' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит SIGNED_METADATA_PURPOSE_OFFSET" >&2
    exit 1
}
grep -Fq 'SIGNED_METADATA_PURPOSE_DENIED' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит SIGNED_METADATA_PURPOSE_DENIED" >&2
    exit 1
}
grep -Fq 'SIGNED_METADATA_MAX_METADATA_LENGTH' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит SIGNED_METADATA_MAX_METADATA_LENGTH" >&2
    exit 1
}
grep -Fq 'struct signed_metadata_layout' \
    kernel/include/signed_metadata.h || {
    echo "signed_metadata.h не содержит struct signed_metadata_layout" >&2
    exit 1
}
for envelope_symbol in signed_metadata_seal signed_metadata_verify \
                       signed_metadata_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $envelope_symbol" >/dev/null || {
        echo "Production kernel не содержит $envelope_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/signed_metadata.c; then
    echo "signed_metadata.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'signed_metadata_host_test_'; then
    echo "signed_metadata_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Signed metadata host tests: ALL OK'; then
    echo "Production kernel содержит signed metadata host marker" >&2
    exit 1
fi
for envelope_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'signed metadata envelope|signed-metadata' \
            "$envelope_doc"; then
        echo "$envelope_doc не документирует signed metadata envelopes" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **143. Signed metadata envelope' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 143 завершённым" >&2
    exit 1
}
grep -Fq -- '- [x] **144. Key-use separation' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 144 завершённым" >&2
    exit 1
}

# Trust anchor rotation staging contracts.
grep -Fq 'test-trust-rotation-host' Makefile || {
    echo "Makefile не содержит test-trust-rotation-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-trust-rotation-host' Makefile || {
    echo "test: target не содержит test-trust-rotation-host" >&2
    exit 1
}
grep -Fq 'TRUST_ROTATION_VERSION' \
    kernel/include/trust_rotation.h || {
    echo "trust_rotation.h не содержит TRUST_ROTATION_VERSION" >&2
    exit 1
}
grep -Fq 'TRUST_ROTATION_NO_PREPARED' \
    kernel/include/trust_rotation.h || {
    echo "trust_rotation.h не содержит TRUST_ROTATION_NO_PREPARED" >&2
    exit 1
}
grep -Fq 'TRUST_ROTATION_COMMIT_FAILED' \
    kernel/include/trust_rotation.h || {
    echo "trust_rotation.h не содержит TRUST_ROTATION_COMMIT_FAILED" >&2
    exit 1
}
grep -Fq 'struct trust_rotation' \
    kernel/include/trust_rotation.h || {
    echo "trust_rotation.h не содержит struct trust_rotation" >&2
    exit 1
}
for rotation_symbol in trust_rotation_init \
                       trust_rotation_stage \
                       trust_rotation_prepare \
                       trust_rotation_activate \
                       trust_rotation_rollback \
                       trust_rotation_has_staged \
                       trust_rotation_has_prepared \
                       trust_rotation_staged \
                       trust_rotation_prepared \
                       trust_rotation_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $rotation_symbol" >/dev/null || {
        echo "Production kernel не содержит $rotation_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(block_device_read|block_device_write)\b' \
        kernel/src/trust_rotation.c; then
    echo "trust_rotation.c выполняет запрещённое выделение, диагностику, аудит или блокирование" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'trust_rotation_host_test_'; then
    echo "trust_rotation_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Trust rotation host tests: ALL OK'; then
    echo "Production kernel содержит trust rotation host marker" >&2
    exit 1
fi
for rotation_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'trust anchor rotation|trust-rotation' \
            "$rotation_doc"; then
        echo "$rotation_doc не документирует trust anchor rotation staging" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **145. Trust-anchor rotation staging' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 145 завершённым" >&2
    exit 1
}

# Local session lifecycle contracts.
grep -Fq 'test-local-session-host' Makefile || {
    echo "Makefile не содержит test-local-session-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-local-session-host' Makefile || {
    echo "test: target не содержит test-local-session-host" >&2
    exit 1
}
grep -Fq 'LOCAL_SESSION_VERSION' \
    kernel/include/local_session.h || {
    echo "local_session.h не содержит LOCAL_SESSION_VERSION" >&2
    exit 1
}
grep -Fq 'LOCAL_SESSION_TABLE_MAX' \
    kernel/include/local_session.h || {
    echo "local_session.h не содержит LOCAL_SESSION_TABLE_MAX" >&2
    exit 1
}
grep -Fq 'struct local_session' \
    kernel/include/local_session.h || {
    echo "local_session.h не содержит struct local_session" >&2
    exit 1
}
grep -Fq 'struct local_session_table' \
    kernel/include/local_session.h || {
    echo "local_session.h не содержит struct local_session_table" >&2
    exit 1
}
for session_symbol in local_session_table_init \
                      local_session_open \
                      local_session_close \
                      local_session_validate \
                      local_session_invalidate_generation \
                      local_session_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $session_symbol" >/dev/null || {
        echo "Production kernel не содержит $session_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b' \
        kernel/src/local_session.c; then
    echo "local_session.c выполняет запрещённое выделение или диагностику" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'local_session_host_test_'; then
    echo "local_session_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Local session host tests: ALL OK'; then
    echo "Production kernel содержит local session host marker" >&2
    exit 1
fi
for session_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'local session|session lifecycle' \
            "$session_doc"; then
        echo "$session_doc не документирует local session lifecycle" >&2
        exit 1
    fi
done

# Failed-authentication throttle contracts.
grep -Fq 'test-auth-throttle-host' Makefile || {
    echo "Makefile не содержит test-auth-throttle-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-auth-throttle-host' Makefile || {
    echo "test: target не содержит test-auth-throttle-host" >&2
    exit 1
}
grep -Fq 'AUTH_THROTTLE_VERSION' \
    kernel/include/auth_throttle.h || {
    echo "auth_throttle.h не содержит AUTH_THROTTLE_VERSION" >&2
    exit 1
}
grep -Fq 'AUTH_THROTTLE_TABLE_MAX' \
    kernel/include/auth_throttle.h || {
    echo "auth_throttle.h не содержит AUTH_THROTTLE_TABLE_MAX" >&2
    exit 1
}
grep -Fq 'AUTH_THROTTLE_FAILURE_LIMIT_MAX' \
    kernel/include/auth_throttle.h || {
    echo "auth_throttle.h не содержит AUTH_THROTTLE_FAILURE_LIMIT_MAX" >&2
    exit 1
}
grep -Fq 'AUTH_THROTTLE_COOLDOWN_MAX' \
    kernel/include/auth_throttle.h || {
    echo "auth_throttle.h не содержит AUTH_THROTTLE_COOLDOWN_MAX" >&2
    exit 1
}
grep -Fq 'struct auth_throttle_table' \
    kernel/include/auth_throttle.h || {
    echo "auth_throttle.h не содержит struct auth_throttle_table" >&2
    exit 1
}
for throttle_symbol in auth_throttle_table_init \
                       auth_throttle_record_failure \
                       auth_throttle_reset \
                       auth_throttle_check \
                       auth_throttle_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $throttle_symbol" >/dev/null || {
        echo "Production kernel не содержит $throttle_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/auth_throttle.c; then
    echo "auth_throttle.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'auth_throttle_host_test_'; then
    echo "auth_throttle_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Auth throttle host tests: ALL OK'; then
    echo "Production kernel содержит auth throttle host marker" >&2
    exit 1
fi
for auth_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'failed authentication throttle|failed-auth throttle|auth throttle|authentication throttle' \
            "$auth_doc"; then
        echo "$auth_doc не документирует failed authentication throttle" >&2
        exit 1
    fi
done

# Management command schema contracts.
grep -Fq 'test-management-command-host' Makefile || {
    echo "Makefile не содержит test-management-command-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-management-command-host' Makefile || {
    echo "test: target не содержит test-management-command-host" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_COMMAND_VERSION' \
    kernel/include/management_command.h || {
    echo "management_command.h не содержит MANAGEMENT_COMMAND_VERSION" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_COMMAND_ID_MIN' \
    kernel/include/management_command.h || {
    echo "management_command.h не содержит MANAGEMENT_COMMAND_ID_MIN" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_COMMAND_PAYLOAD_MAX' \
    kernel/include/management_command.h || {
    echo "management_command.h не содержит MANAGEMENT_COMMAND_PAYLOAD_MAX" >&2
    exit 1
}
grep -Fq 'MANAGEMENT_COMMAND_HEADER_LENGTH' \
    kernel/include/management_command.h || {
    echo "management_command.h не содержит MANAGEMENT_COMMAND_HEADER_LENGTH" >&2
    exit 1
}
grep -Fq 'struct management_command_registry' \
    kernel/include/management_command.h || {
    echo "management_command.h не содержит struct management_command_registry" >&2
    exit 1
}
for command_symbol in management_command_registry_init \
                      management_command_validate_frame \
                      management_command_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $command_symbol" >/dev/null || {
        echo "Production kernel не содержит $command_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/management_command.c; then
    echo "management_command.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'management_command_host_test_'; then
    echo "management_command_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Management command host tests: ALL OK'; then
    echo "Production kernel содержит management command host marker" >&2
    exit 1
fi
for command_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'management command schema|management-command' \
            "$command_doc"; then
        echo "$command_doc не документирует management command schema" >&2
        exit 1
    fi
done

# Authorization matrix contracts.
grep -Fq 'test-authorization-host' Makefile || {
    echo "Makefile не содержит test-authorization-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-authorization-host' Makefile || {
    echo "test: target не содержит test-authorization-host" >&2
    exit 1
}
grep -Fq 'AUTHORIZATION_VERSION' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит AUTHORIZATION_VERSION" >&2
    exit 1
}
grep -Fq 'AUTHORIZATION_MATRIX_CAPACITY_MAX' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит AUTHORIZATION_MATRIX_CAPACITY_MAX" >&2
    exit 1
}
grep -Fq 'AUTHORIZATION_ROLE_SET_MAX' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит AUTHORIZATION_ROLE_SET_MAX" >&2
    exit 1
}
grep -Fq 'AUTHORIZATION_CAPABILITY_SET_MAX' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит AUTHORIZATION_CAPABILITY_SET_MAX" >&2
    exit 1
}
grep -Fq 'struct authorization_matrix' \
    kernel/include/authorization.h || {
    echo "authorization.h не содержит struct authorization_matrix" >&2
    exit 1
}
for authorization_symbol in authorization_matrix_init \
                           authorization_check \
                           authorization_status_string; do
    nm build/hfoc-kernel.elf | grep -F " $authorization_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $authorization_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/authorization.c; then
    echo "authorization.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'authorization_host_test_'; then
    echo "authorization_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Authorization matrix host tests: ALL OK'; then
    echo "Production kernel содержит authorization matrix host marker" >&2
    exit 1
fi
for authorization_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'authorization matrix|authorization-matrix' \
            "$authorization_doc"; then
        echo "$authorization_doc не документирует authorization matrix" >&2
        exit 1
    fi
done

# Read-only appliance status command contracts.
grep -Fq 'test-appliance-status-host' Makefile || {
    echo "Makefile не содержит test-appliance-status-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-status-host' Makefile || {
    echo "test: target не содержит test-appliance-status-host" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATUS_VERSION' \
    kernel/include/appliance_status.h || {
    echo "appliance_status.h не содержит APPLIANCE_STATUS_VERSION" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATUS_COMMAND_ID' \
    kernel/include/appliance_status.h || {
    echo "appliance_status.h не содержит APPLIANCE_STATUS_COMMAND_ID" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATUS_SERVICE_MAX' \
    kernel/include/appliance_status.h || {
    echo "appliance_status.h не содержит APPLIANCE_STATUS_SERVICE_MAX" >&2
    exit 1
}
grep -Fq 'struct appliance_status_snapshot' \
    kernel/include/appliance_status.h || {
    echo "appliance_status.h не содержит struct appliance_status_snapshot" >&2
    exit 1
}
for status_symbol in appliance_status_snapshot_init \
                     appliance_status_render \
                     appliance_status_command_descriptor \
                     appliance_status_authorization_entry \
                     appliance_status_status_string \
                     appliance_status_health_string \
                     appliance_status_storage_string \
                     appliance_status_service_state_string \
                     appliance_status_service_health_string; do
    nm build/hfoc-kernel.elf | grep -F " $status_symbol" \
        >/dev/null || {
        echo "Production kernel не содержит $status_symbol" >&2
        exit 1
    }
done
if grep -nE '\b(kmalloc|kfree|pmm_alloc_contiguous_pages|pmm_free_contiguous_pages)\b|\b(serial_printf|printf|fprintf|audit_|audit_record|panic)\b|\b(sleep|delay|hlt|sti|cli|pic_|lapic_|ioapic_|smp_)\b' \
        kernel/src/appliance_status.c; then
    echo "appliance_status.c выполняет запрещённое выделение, диагностику или блокировку" >&2
    exit 1
fi
if nm -g build/hfoc-kernel.elf | grep 'appliance_status_host_test_'; then
    echo "appliance_status_host_test_ символы попали в kernel ELF" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        'Appliance status host tests: ALL OK'; then
    echo "Production kernel содержит appliance status host marker" >&2
    exit 1
fi
for status_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'read-only appliance status|appliance status command' \
            "$status_doc"; then
        echo "$status_doc не документирует read-only appliance status command" >&2
        exit 1
    fi
done
