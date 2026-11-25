#!/usr/bin/env bash
# ITEM-506 increment 5: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.

# The management plane serves only a machine that has an account.
#
# The unattended install leaves the account unset, so the first boot of an
# installed appliance used to raise the listener with no way to sign in.  The
# gate withholds the listener while the borrowed state loader is ready and the
# one account (login + PBKDF2 verifier) does not exist; the console conversation
# sets it and the next poll raises the listener.  A boot with no ready state
# volume keeps the documented per-boot-certificate behaviour.  The claims pinned
# here:
#
#   * the enrollment owner exposes one named predicate, and only a structured
#     account (not an absent store, not the factory placeholder, not a
#     pre-ITEM-506 legacy fingerprint) allows serving;
#   * the serving state machine gained AWAITING_CREDENTIAL as an appended enum
#     value, renders it, and publishes `reason=admin_credential_unset` with the
#     listener down, before the clock, network, entropy and certificate steps;
#   * the host tests prove the predicate (all four store heads) and the
#     installed-state transition before/after the account commit;
#   * the serving proof waits for the withheld listener first, sets the account
#     on the console, and only then runs the TLS client; the five managed
#     targets answer anonymously with 401 after the set and with the
#     gate-unavailable 503 on the network-apply boot, which publishes no
#     verifier yet;
#   * the forward proof sets the account on the installed appliance's console
#     before its relay wait, with a CPU that carries the hardware RNG the
#     console salt needs;
#   * the network proof (no state volume) still serves with a per-boot
#     certificate and is untouched.
for item506_inc5_file in \
    kernel/include/appliance_production_serving.h \
    kernel/src/appliance_production_serving.c \
    kernel/include/appliance_production_admin_enrollment.h \
    kernel/src/appliance_production_admin_enrollment.c \
    kernel/src/appliance_state_loader.c \
    tests/appliance-production-admin-enrollment-host.c \
    tests/production-installer-orchestration-host.c \
    tests/appliance-production-first-boot-host.c \
    scripts/test-qemu-production-service-https.sh \
    scripts/test-qemu-production-forward.sh \
    scripts/test-qemu-production-network.sh \
    docs/ARCHITECTURE.md \
    docs/hfoc-production-admin-enrollment.md \
    docs/ROADMAP.md; do
    [[ -f $item506_inc5_file ]] || {
        echo "ITEM-506 inc5: отсутствует $item506_inc5_file" >&2
        exit 1
    }
done

# The predicate is one named reader, and it is the credential reader.
for item506_inc5_predicate in \
    'appliance_production_admin_enrollment_serving_allowed(' \
    'return appliance_production_admin_enrollment_credential_set(enrollment);'; do
    grep -Fq -- "$item506_inc5_predicate" \
        kernel/include/appliance_production_admin_enrollment.h \
        kernel/src/appliance_production_admin_enrollment.c || {
        echo "ITEM-506 inc5: нет предиката обслуживания:" \
            "$item506_inc5_predicate" >&2
        exit 1
    }
done

# The state is appended, rendered, and the gate runs before every other wait.
grep -Fq 'APPLIANCE_PRODUCTION_SERVING_STATE_AWAITING_CREDENTIAL = 8,' \
    kernel/include/appliance_production_serving.h || {
    echo "ITEM-506 inc5: состояние ожидания учётной записи не добавлено в конец" >&2
    exit 1
}
grep -Fq 'return "AWAITING_CREDENTIAL";' \
    kernel/src/appliance_production_serving.c || {
    echo "ITEM-506 inc5: состояние не рендерится" >&2
    exit 1
}
for item506_inc5_gate in \
    'appliance_state_loader_ready(serving->loader)' \
    'appliance_production_admin_enrollment_serving_allowed(' \
    '? "admin_enrollment_unbound"' \
    ': "admin_credential_unset";'; do
    grep -Fq -- "$item506_inc5_gate" \
        kernel/src/appliance_production_serving.c || {
        echo "ITEM-506 inc5: нет гейта первого запуска:" \
            "$item506_inc5_gate" >&2
        exit 1
    }
done
gate_line=$(grep -n 'admin_credential_unset' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
clock_line=$(grep -n '"clock_unavailable"' \
    kernel/src/appliance_production_serving.c | head -1 | cut -d: -f1)
[[ -n $gate_line && -n $clock_line && $gate_line -lt $clock_line ]] || {
    echo "ITEM-506 inc5: гейт стоит не перед проверкой часов" >&2
    exit 1
}

# The host proofs cover the predicate, the installed-state transition and the
# deferred first boot's own reading.
for item506_inc5_host in \
    'test_first_run_serving_gate' \
    'the factory placeholder withholds serving' \
    'a legacy fingerprint withholds serving' \
    'the structured account allows serving'; do
    grep -Fq -- "$item506_inc5_host" \
        tests/appliance-production-admin-enrollment-host.c || {
        echo "ITEM-506 inc5: host-проба предиката не проверяет:" \
            "$item506_inc5_host" >&2
        exit 1
    }
done
for item506_inc5_orchestration in \
    'test_installed_state_first_run_gate' \
    'an installed unattended state carries no account to serve with' \
    'the installed system'"'"'s own reader now finds the account'; do
    grep -Fq -- "$item506_inc5_orchestration" \
        tests/production-installer-orchestration-host.c || {
        echo "ITEM-506 inc5: проба установленного состояния не проверяет:" \
            "$item506_inc5_orchestration" >&2
        exit 1
    }
done
grep -Fq 'deferred first boot leaves the serving gate without an account' \
    tests/appliance-production-first-boot-host.c || {
    echo "ITEM-506 inc5: отложенная первая загрузка не читает гейт" >&2
    exit 1
}

# The serving proof: withheld first, set, live, client last.
for item506_inc5_https in \
    'state=AWAITING_CREDENTIAL reason=admin_credential_unset listener=down' \
    'TLS-PROOF serving gate: listener withheld until the account is set' \
    'TLS-PROOF serving gate: listener live after the account was set' \
    'read-only management targets: known routes, anonymous denial 401' \
    'HFOC_PRODUCTION_SERVICE_TEST_TIMEOUT:-1800' \
    '"network-apply boot" /api/v1/system /api/v1/network'; do
    grep -Fq -- "$item506_inc5_https" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-506 inc5: проба сервиса не проверяет:" \
            "$item506_inc5_https" >&2
        exit 1
    }
done
conversation_line=$(grep -n 'ADMIN_SECRET=hfocsecret401' \
    scripts/test-qemu-production-service-https.sh | head -1 | cut -d: -f1)
client_line=$(grep -n 'serving_client_status=0' \
    scripts/test-qemu-production-service-https.sh | head -1 | cut -d: -f1)
[[ -n $conversation_line && -n $client_line &&
    $conversation_line -lt $client_line ]] || {
    echo "ITEM-506 inc5: TLS-клиент не переставлен после разговора" >&2
    exit 1
}
if grep -Fq 'if status != 503:' \
    <(sed -n "${client_line},$((client_line + 40))p" \
        scripts/test-qemu-production-service-https.sh); then
    echo "ITEM-506 inc5: анонимные цели всё ещё ждут 503 после установки" >&2
    exit 1
fi

# The forward proof sets the account before its relay wait, on a CPU that has
# the hardware RNG.
# shellcheck disable=SC2016  # the compared text is literal source
for item506_inc5_forward in \
    'forward-enroll.py' \
    'the first-run account is set' \
    '[ADMIN-ENROLLMENT] credentials changed' \
    '-cpu "${QEMU_CPU:-max}"' \
    'the hidden console input leaked the password'; do
    grep -Fq -- "$item506_inc5_forward" \
        scripts/test-qemu-production-forward.sh || {
        echo "ITEM-506 inc5: проба ретрансляции не проверяет:" \
            "$item506_inc5_forward" >&2
        exit 1
    }
done

# The no-volume behaviour is the untouched network proof: no state drive, and
# the listener still reaches LISTENING.
if grep -Eq -- '(^|[[:space:]])-drive([[:space:]]|$)' \
    scripts/test-qemu-production-network.sh; then
    echo "ITEM-506 inc5: у пробы без тома появился диск состояния" >&2
    exit 1
fi
grep -Fq 'state=LISTENING' scripts/test-qemu-production-network.sh || {
    echo "ITEM-506 inc5: проба без тома больше не ждёт LISTENING" >&2
    exit 1
}

# The rule is documented where a maintainer and the operator look for it.
for item506_inc5_doc in \
    'AWAITING_CREDENTIAL' \
    'ITEM-506'; do
    grep -Fq -- "$item506_inc5_doc" docs/ARCHITECTURE.md || {
        echo "docs/ARCHITECTURE.md не фиксирует ITEM-506 inc5:" \
            "$item506_inc5_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-506' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-506" >&2
    exit 1
}
grep -Fq 'ITEM-506' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-506" >&2
    exit 1
}
