#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Storage encryption and key custody contracts (ITEM-373).
for state_encryption_file in \
    kernel/include/appliance_state_crypto.h \
    kernel/src/appliance_state_crypto.c \
    kernel/include/appliance_key_custody.h \
    kernel/src/appliance_key_custody.c \
    kernel/include/appliance_state_encryption.h \
    kernel/src/appliance_state_encryption.c \
    kernel/include/hfoc_state_crypto_config.h \
    tests/state-encryption-host.c \
    docs/hfoc-state-encryption-key-custody.md; do
    test -f "$state_encryption_file" || {
        echo "Отсутствует ITEM-373 file: $state_encryption_file" >&2
        exit 1
    }
done
grep -Fq 'test-state-encryption-host' Makefile || {
    echo "Makefile не содержит test-state-encryption-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-state-encryption-host' Makefile || {
    echo "test-host-suite не вызывает test-state-encryption-host" >&2
    exit 1
}
grep -Fq 'HFOC_STATE_CRYPTO_MBEDTLS_CFLAGS' Makefile || {
    echo "Makefile не содержит bounded state crypto profile" >&2
    exit 1
}
grep -Fq 'third_party/mbedtls/library/aes.c' Makefile || {
    echo "state crypto profile не называет единственный cipher source" >&2
    exit 1
}
grep -Fq 'MBEDTLS_AES_C' kernel/include/hfoc_state_crypto_config.h || {
    echo "state crypto config не включает AES" >&2
    exit 1
}
if grep -Eq '^#define MBEDTLS_(SSL|X509|PSA|PK|MD|HKDF|CTR_DRBG|GCM|BIGNUM)_' \
    kernel/include/hfoc_state_crypto_config.h; then
    echo "state crypto config включает запрещённое семейство" >&2
    exit 1
fi
grep -Fq 'APPLIANCE_KEY_CUSTODY_TPM_UNAVAILABLE' \
    kernel/src/appliance_key_custody.c || {
    echo "key custody не фиксирует TPM UNAVAILABLE" >&2
    exit 1
}
grep -Fq 'APPLIANCE_KEY_CUSTODY_WRONG_KEY' \
    kernel/src/appliance_key_custody.c || {
    echo "key custody не фиксирует wrong-key fail-closed" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATE_ENCRYPTION_AUTH_FAILED' \
    kernel/src/appliance_state_encryption.c || {
    echo "state encryption не фиксирует authentication failure" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATE_LOADER_KEY_REQUIRED' \
    kernel/src/appliance_state_loader.c || {
    echo "state loader не публикует KEY_REQUIRED" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATE_LOADER_KEY_REJECTED' \
    kernel/src/appliance_state_loader.c || {
    echo "state loader не публикует KEY_REJECTED" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_BOOT_STATE_LOCKED' \
    kernel/src/appliance_production_boot.c || {
    echo "production boot не публикует honest STATE_LOCKED" >&2
    exit 1
}
grep -Fq 'APPLIANCE_PRODUCTION_BOOT_STATE_KEY_REJECTED' \
    kernel/src/appliance_production_boot.c || {
    echo "production boot не публикует honest STATE_KEY_REJECTED" >&2
    exit 1
}
grep -Fq 'HFOC-RECOVERY-KEY' kernel/src/appliance_key_custody.c || {
    echo "key custody не публикует escrow line" >&2
    exit 1
}
grep -Fq 'HMAC-SHA-256' docs/hfoc-state-encryption-key-custody.md || {
    echo "ITEM-373 doc не документирует MAC" >&2
    exit 1
}
grep -Fq 'Appliance state encryption host tests: ALL OK' \
    tests/state-encryption-host.c || {
    echo "host proof не печатает marker" >&2
    exit 1
}
if nm build/hfoc-kernel.elf | grep -F ' appliance_state_crypto_ctr' >/dev/null; then
    :
else
    echo "production kernel не содержит state crypto backend" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf 'state encryption host tests'; then
    echo "Production kernel содержит state encryption proof marker" >&2
    exit 1
fi
grep -Fq 'stage_17_state=DEGRADED' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "STORAGE_ENCRYPTION stage не помечен DEGRADED" >&2
    exit 1
}
grep -Fq 'stage_17_target=test-state-encryption-host' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "STORAGE_ENCRYPTION stage не называет focused proof" >&2
    exit 1
}
grep -Fq 'stage_17_marker=Appliance state encryption host tests: ALL OK' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "STORAGE_ENCRYPTION stage не фиксирует marker" >&2
    exit 1
}
for state_encryption_doc in \
    docs/hfoc-state-encryption-key-custody.md \
    docs/hfoc-production-end-to-end-acceptance.md; do
    grep -Fq 'test-state-encryption-host' "$state_encryption_doc" || {
        echo "$state_encryption_doc не документирует focused proof" >&2
        exit 1
    }
done
for state_encryption_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'storage encryption|state-encryption-key-custody' \
        "$state_encryption_metadata_doc" || {
        echo "$state_encryption_metadata_doc не документирует storage encryption" >&2
        exit 1
    }
done

# Network modes, time synchronization and offline bring-up contracts (ITEM-374).
for network_time_file in \
    kernel/include/appliance_time_sync.h \
    kernel/src/appliance_time_sync.c \
    kernel/include/appliance_time_wire.h \
    kernel/src/appliance_time_wire.c \
    kernel/include/appliance_network_mode.h \
    kernel/src/appliance_network_mode.c \
    kernel/include/appliance_management_bind.h \
    kernel/src/appliance_management_bind.c \
    tests/appliance-time-sync-host.c \
    tests/appliance-network-mode-host.c \
    docs/hfoc-production-network-time-offline.md; do
    test -f "$network_time_file" || {
        echo "Отсутствует ITEM-374 file: $network_time_file" >&2
        exit 1
    }
done
for network_time_target in \
    test-appliance-time-sync-host \
    test-appliance-network-mode-host; do
    grep -Fq "$network_time_target" Makefile || {
        echo "Makefile не содержит $network_time_target" >&2
        exit 1
    }
    # shellcheck disable=SC2016
    grep -Fq "\$(MAKE) $network_time_target" Makefile || {
        echo "test-host-suite не вызывает $network_time_target" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE' \
    kernel/src/appliance_network_config.c || {
    echo "network config не публикует offline mode" >&2
    exit 1
}
grep -Fq 'APPLIANCE_NETWORK_MODE_ACTION_OFFLINE' \
    kernel/src/appliance_network_mode.c || {
    echo "mode decision не фиксирует offline" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY' \
    kernel/src/appliance_management_bind.c || {
    echo "management bind не публикует local-only default" >&2
    exit 1
}
grep -Fq 'APPLIANCE_TIME_SYNC_STATE_SYNCHRONIZED' \
    kernel/src/appliance_time_sync.c || {
    echo "time sync не публикует synchronized state" >&2
    exit 1
}
grep -Fq 'appliance_production_service_boot_poll_clock' \
    kernel/src/kernel.c || {
    echo "kernel не применяет clock gate к TLS activation" >&2
    exit 1
}
grep -Fq 'PRODUCTION-NET-MODE' kernel/src/kernel.c || {
    echo "kernel не публикует network mode line" >&2
    exit 1
}
grep -Fq 'PRODUCTION-MGMT-BIND' kernel/src/kernel.c || {
    echo "kernel не публикует management bind line" >&2
    exit 1
}
grep -Fq 'QEMU production network/time test: OK' \
    scripts/test-qemu-production-dhcp-dns.sh || {
    echo "QEMU proof не публикует network/time marker" >&2
    exit 1
}
grep -Fq 'clock=READY' scripts/test-qemu-production-dhcp-dns.sh || {
    echo "QEMU proof не требует synchronized clock" >&2
    exit 1
}
bash -n scripts/test-qemu-production-dhcp-dns.sh || {
    echo "Синтаксис scripts/test-qemu-production-dhcp-dns.sh некорректен" >&2
    exit 1
}
grep -Fq 'Appliance time sync host tests: ALL OK' \
    tests/appliance-time-sync-host.c || {
    echo "time sync host proof не печатает marker" >&2
    exit 1
}
grep -Fq 'Appliance network mode host tests: ALL OK' \
    tests/appliance-network-mode-host.c || {
    echo "network mode host proof не печатает marker" >&2
    exit 1
}
if nm build/hfoc-kernel.elf | grep -F ' appliance_time_sync_input' >/dev/null; then
    :
else
    echo "production kernel не содержит time sync backend" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf 'network mode host tests'; then
    echo "Production kernel содержит network mode proof marker" >&2
    exit 1
fi
grep -Fq 'stage_18_state=DEGRADED' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "TIME_SYNC stage не помечен DEGRADED" >&2
    exit 1
}
grep -Fq 'stage_18_target=test-appliance-network-mode-host' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "TIME_SYNC stage не называет focused proof" >&2
    exit 1
}
grep -Fq 'stage_18_marker=Appliance network mode host tests: ALL OK' \
    release/hfoc-production-end-to-end-acceptance.txt || {
    echo "TIME_SYNC stage не фиксирует marker" >&2
    exit 1
}
for network_time_doc in \
    docs/hfoc-production-network-time-offline.md \
    docs/hfoc-production-end-to-end-acceptance.md; do
    grep -Fq 'test-appliance-network-mode-host' "$network_time_doc" || {
        echo "$network_time_doc не документирует focused proof" >&2
        exit 1
    }
done
for network_time_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'network modes|time synchronization|offline bring-up|production-network-time-offline' \
        "$network_time_metadata_doc" || {
        echo "$network_time_metadata_doc не документирует network/time/offline" >&2
        exit 1
    }
done

# Trademark and certified-build policy contracts (ITEM-383).
for trademark_policy_file in \
    docs/hfoc-trademark-and-certified-build-policy.md \
    release/trademark-and-certified-build-policy.txt \
    scripts/verify-trademark-policy.sh \
    tests/test-trademark-policy-host.sh; do
    test -f "$trademark_policy_file" || {
        echo "Отсутствует ITEM-383 file: $trademark_policy_file" >&2
        exit 1
    }
done
for trademark_policy_executable in \
    scripts/verify-trademark-policy.sh \
    tests/test-trademark-policy-host.sh; do
    test -x "$trademark_policy_executable" || {
        echo "$trademark_policy_executable не исполняемый" >&2
        exit 1
    }
    bash -n "$trademark_policy_executable" || {
        echo "Синтаксис ${trademark_policy_executable} некорректен" >&2
        exit 1
    }
done
grep -Fq 'test-trademark-policy-host:' Makefile || {
    echo "Makefile не содержит test-trademark-policy-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-trademark-policy-host' Makefile || {
    echo "test-host-suite не вызывает test-trademark-policy-host" >&2
    exit 1
}
bash scripts/verify-trademark-policy.sh \
    release/trademark-and-certified-build-policy.txt >/dev/null
# The host proof owns the negative fixtures: it refuses a weakened artefact, an
# unknown or missing section and a missing label field.
bash tests/test-trademark-policy-host.sh >/dev/null

# Answer 1: which names and marks may be used.
for trademark_name in 'HFOC OS' 'HFOC Appliance' 'HFOC Certified'; do
    grep -Fq -- "$trademark_name" \
        docs/hfoc-trademark-and-certified-build-policy.md || {
        echo "ITEM-383 doc не называет mark: $trademark_name" >&2
        exit 1
    }
done
# shellcheck disable=SC2016
for trademark_use_fragment in \
    'nominative fair use' \
    'applying `HFOC Certified` to a build that has no valid certified-build label' \
    'removing or obscuring HFOC attribution, licence notices or the build identity' \
    'No mark right travels with the code licence.'; do
    grep -Fq -- "$trademark_use_fragment" \
        docs/hfoc-trademark-and-certified-build-policy.md || {
        echo "ITEM-383 doc не фиксирует правила использования marks" >&2
        exit 1
    }
done

# Answer 2: how a certified build is identified and labelled.
for trademark_label_field in \
    release_id \
    source_revision \
    image_sha256 \
    release_gate_report \
    sbom_digest \
    acceptance_report \
    signer_anchor; do
    grep -Fq -- "\`$trademark_label_field\`" \
        docs/hfoc-trademark-and-certified-build-policy.md || {
        echo "ITEM-383 doc не называет поле certified-build label: $trademark_label_field" >&2
        exit 1
    }
    grep -Fq -- "certified_label_${trademark_label_field}=" \
        release/trademark-and-certified-build-policy.txt || {
        echo "ITEM-383 artefact не содержит поле certified-build label: $trademark_label_field" >&2
        exit 1
    }
done
grep -Fq -- 'What makes a label invalid' \
    docs/hfoc-trademark-and-certified-build-policy.md || {
    echo "ITEM-383 doc не фиксирует недействительность certified-build label" >&2
    exit 1
}

# Answer 3: what an OEM or integrator may redistribute, and what travels.
grep -Fq -- '## 3. OEM and integrator redistribution' \
    docs/hfoc-trademark-and-certified-build-policy.md || {
    echo "ITEM-383 doc не содержит раздел redistribution" >&2
    exit 1
}
for trademark_redistribution_fragment in \
    'LICENSES.sha256' \
    'pass these obligations to every further recipient' \
    'SBOM of the redistributed build'; do
    grep -Fq -- "$trademark_redistribution_fragment" \
        docs/hfoc-trademark-and-certified-build-policy.md || {
        echo "ITEM-383 doc не фиксирует обязанность redistribution: $trademark_redistribution_fragment" >&2
        exit 1
    }
done
grep -Fq 'redistribution_requires_written_agreement=true' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact не требует письменного соглашения" >&2
    exit 1
}

# Answer 4: what the community build may not claim.
for trademark_community_fragment in \
    'must not claim to be a certified build' \
    'must not claim vendor support, warranty, an SLA or an official update channel' \
    'must not use the HFOC marks beyond nominative fair use'; do
    grep -Fq -- "$trademark_community_fragment" \
        docs/hfoc-trademark-and-certified-build-policy.md || {
        echo "ITEM-383 doc не фиксирует запрет community build: $trademark_community_fragment" >&2
        exit 1
    }
done
grep -Fq 'community_build_may_claim_certified_build=false' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact разрешает community build заявлять сертификацию" >&2
    exit 1
}
grep -Fq 'community_build_mark_use=nominative_fair_use_only' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact не ограничивает использование marks community build" >&2
    exit 1
}
grep -Fq 'community_build_may_claim_vendor_support=false' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact разрешает community build заявлять поддержку вендора" >&2
    exit 1
}

# The repository publishes no certification claim it cannot verify, and the
# policy is not legal advice: the executed agreement is the binding instrument.
grep -Fq 'policy_certified_build_published=false' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact фабрикует сертифицированный build" >&2
    exit 1
}
grep -Fq 'legal_advice=false' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact выдаёт политику за юридическую консультацию" >&2
    exit 1
}
grep -Fq 'legal_review_required=true' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact не требует юридической проверки" >&2
    exit 1
}
for trademark_caveat_fragment in \
    'This policy is not legal advice.' \
    'the executed commercial agreement, not this policy,' \
    'is the binding instrument'; do
    grep -Fq -- "$trademark_caveat_fragment" \
        docs/hfoc-trademark-and-certified-build-policy.md || {
        echo "ITEM-383 doc не фиксирует: $trademark_caveat_fragment" >&2
        exit 1
    }
done
grep -Fq 'COMMERCIAL-LICENSING.md' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact не называет binding instrument" >&2
    exit 1
}

# The release gate of ITEM-380 owns the artefacts a label references; the
# policy names them by role and must not pretend they already exist.
grep -Fq 'policy_release_gate_item=ITEM-380' \
    release/trademark-and-certified-build-policy.txt || {
    echo "ITEM-383 artefact не ссылается на release gate ITEM-380" >&2
    exit 1
}
grep -Fq 'ITEM-380' docs/hfoc-trademark-and-certified-build-policy.md || {
    echo "ITEM-383 doc не ссылается на release gate ITEM-380" >&2
    exit 1
}

for trademark_policy_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'trademark and certified-build policy|trademark and certified build policy' \
        "$trademark_policy_doc"; then
        echo "$trademark_policy_doc не документирует trademark и certified-build policy" >&2
        exit 1
    fi
    if ! grep -Eiq 'certified build' "$trademark_policy_doc"; then
        echo "$trademark_policy_doc не документирует certified build" >&2
        exit 1
    fi
done
grep -Fq -- '- ITEM-383 (реализован): trademark and certified-build policy' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "productization queue не помечает ITEM-383 реализованным" >&2
    exit 1
}
grep -Fq 'docs/hfoc-trademark-and-certified-build-policy.md' \
    docs/hfoc-appliance-licensing-and-certification.md || {
    echo "licensing memo D2 не ссылается на политику" >&2
    exit 1
}

# Management reachability policy contracts (ITEM-385).
for reachability_file in \
    kernel/include/appliance_management_reachability.h \
    kernel/src/appliance_management_reachability.c \
    kernel/src/appliance_management_reachability_qemu.c \
    tests/appliance-management-reachability-host.c \
    docs/hfoc-management-reachability-policy.md; do
    test -f "$reachability_file" || {
        echo "Отсутствует ITEM-385 file: $reachability_file" >&2
        exit 1
    }
done
grep -Fq 'test-appliance-management-reachability-host' Makefile || {
    echo "Makefile не содержит test-appliance-management-reachability-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-appliance-management-reachability-host' Makefile || {
    echo "test-host-suite не вызывает test-appliance-management-reachability-host" >&2
    exit 1
}
for reachability_mode in \
    APPLIANCE_MANAGEMENT_REACHABILITY_MODE_CONSOLE_ONLY \
    APPLIANCE_MANAGEMENT_REACHABILITY_MODE_MANAGEMENT_INTERFACE \
    APPLIANCE_MANAGEMENT_REACHABILITY_MODE_REMOTE_OPT_IN; do
    grep -Fq "$reachability_mode" \
        kernel/include/appliance_management_reachability.h || {
        echo "reachability policy не содержит режим $reachability_mode" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_MANAGEMENT_REACHABILITY_ALLOWLIST_MAX 4U' \
    kernel/include/appliance_management_reachability.h || {
    echo "reachability allowlist не ограничен" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_REACHABILITY_INTERFACE_NONE UINT32_C(0xFFFFFFFF)' \
    kernel/include/appliance_management_reachability.h || {
    echo "reachability policy не различает отсутствие management interface" >&2
    exit 1
}
# The console is the break-glass path and must be decided before publication,
# otherwise a missing policy would lock the operator out of the appliance.
reachability_console_line=$(grep -n 'peer->console' \
    kernel/src/appliance_management_reachability.c | head -n 1 | cut -d: -f1)
reachability_published_line=$(grep -n 'selection_published(selection)' \
    kernel/src/appliance_management_reachability.c | tail -n 1 | cut -d: -f1)
test -n "$reachability_console_line" || {
    echo "reachability evaluator не рассматривает console peer" >&2
    exit 1
}
test -n "$reachability_published_line" || {
    echo "reachability evaluator не проверяет публикацию policy" >&2
    exit 1
}
test "$reachability_console_line" -lt "$reachability_published_line" || {
    echo "reachability evaluator проверяет публикацию раньше console break-glass" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_OUTER_INTERFACE' \
    kernel/src/appliance_management_reachability.c || {
    echo "reachability evaluator не отклоняет outer interface" >&2
    exit 1
}
grep -Fq 'APPLIANCE_MANAGEMENT_REACHABILITY_VERDICT_DENIED_NO_CLIENT_CERTIFICATE' \
    kernel/src/appliance_management_reachability.c || {
    echo "reachability evaluator не требует client certificate" >&2
    exit 1
}
# Reachability must gate the live request path, not exist as an unused module.
grep -Fq '#include "appliance_management_reachability.h"' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "live HTTPS path не включает reachability policy" >&2
    exit 1
}
grep -Fq 'vnf_https_qemu_reachability_admit(connection)' \
    kernel/src/virtio_net_frame_qemu.c || {
    echo "live HTTPS path не вызывает reachability gate" >&2
    exit 1
}
for reachability_marker in \
    '[MGMT-REACH] proof init' \
    '[MGMT-REACH] console allowed ok' \
    '[MGMT-REACH] outer interface denied ok' \
    '[MGMT-REACH] management interface served ok' \
    '[MGMT-REACH] live peer admitted ok' \
    '[MGMT-REACH] live peer denied ok' \
    '[MGMT-REACH] proof OK'; do
    grep -Fq "$reachability_marker" \
        scripts/test-qemu-management-plane-acceptance.sh || {
        echo "QEMU proof не проверяет marker: $reachability_marker" >&2
        exit 1
    }
done
grep -Fq 'production kernel contains reachability marker' Makefile || {
    echo "Makefile не запрещает reachability marker в production kernel" >&2
    exit 1
}
grep -Fq 'single network interface' \
    docs/hfoc-management-reachability-policy.md || {
    echo "ITEM-385 doc не фиксирует одноинтерфейсное ограничение proof" >&2
    exit 1
}
for reachability_metadata_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    grep -Eiq 'management reachability|reachability policy' \
        "$reachability_metadata_doc" || {
        echo "$reachability_metadata_doc не документирует management reachability" >&2
        exit 1
    }
done

echo "HFOC OS 1.0.0 checks: OK"
